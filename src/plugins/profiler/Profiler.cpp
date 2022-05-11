/*
 *  The contents of this file are subject to the Initial
 *  Developer's Public License Version 1.0 (the "License");
 *  you may not use this file except in compliance with the
 *  License. You may obtain a copy of the License at
 *  http://www.ibphoenix.com/main.nfs?a=ibphoenix&page=ibp_idpl.
 *
 *  Software distributed under the License is distributed AS IS,
 *  WITHOUT WARRANTY OF ANY KIND, either express or implied.
 *  See the License for the specific language governing rights
 *  and limitations under the License.
 *
 *  The Original Code was created by Adriano dos Santos Fernandes
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2020 Adriano dos Santos Fernandes <adrianosf@gmail.com>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 */

#include "firebird.h"
#include "firebird/Message.h"
#include "../common/classes/ImplementHelper.h"
#include "../common/classes/auto.h"
#include "../common/classes/fb_pair.h"
#include "../common/classes/fb_string.h"
#include "../common/classes/GenericMap.h"
#include "../common/classes/MetaString.h"
#include "../common/classes/Nullable.h"
#include "../common/classes/objects_array.h"
#include "../common/classes/stack.h"
#include "../common/status.h"
#include "../intl/charsets.h"
#include "../jrd/intl.h"
#include <unicode/utf8.h>

using namespace Firebird;


namespace
{

class ProfilerPlugin;

auto& defaultPool()
{
	return *getDefaultMemoryPool();
}

void quote(string& name)
{
	const char QUOTE = '"';

	for (unsigned p = 0; p < name.length(); ++p)
	{
		if (name[p] == QUOTE)
		{
			name.insert(p, 1, QUOTE);
			++p;
		}
	}

	name.insert(0u, 1, QUOTE);
	name += QUOTE;
}

struct Stats
{
	void hit(FB_UINT64 runTime)
	{
		if (counter == 0 || runTime < minTime)
			minTime = runTime;

		if (counter == 0 || runTime > maxTime)
			maxTime = runTime;

		totalTime += runTime;
		++counter;
	}

	FB_UINT64 counter = 0;
	FB_UINT64 minTime = 0;
	FB_UINT64 maxTime = 0;
	FB_UINT64 totalTime = 0;
};

struct RecordSource
{
	Nullable<ULONG> parentId;
	string accessPath{defaultPool()};
};

struct RecordSourceStats
{
	Stats openStats;
	Stats fetchStats;
};

struct Statement
{
	unsigned level = 0;
	string type{defaultPool()};
	MetaString packageName{defaultPool()};
	MetaString routineName{defaultPool()};
	SINT64 parentStatementId;
	string sqlText{defaultPool()};
};

using LineColumnKey = NonPooledPair<unsigned, unsigned>;
using CursorRecSourceKey = NonPooledPair<unsigned, unsigned>;

struct Request
{
	bool dirty = true;
	unsigned level = 0;
	SINT64 statementId;
	SINT64 callerRequestId;
	ISC_TIMESTAMP_TZ startTimestamp;
	Nullable<ISC_TIMESTAMP_TZ> finishTimestamp;
	NonPooledMap<CursorRecSourceKey, RecordSourceStats> recordSourcesStats{defaultPool()};
	NonPooledMap<LineColumnKey, Stats> psqlStats{defaultPool()};
};

using StatementCursorRecSourceKey = NonPooledPair<NonPooledPair<SINT64, unsigned>, unsigned>;

class Session final :
	public IProfilerSessionImpl<Session, ThrowStatusExceptionWrapper>,
	public RefCounted
{
public:
	Session(ThrowStatusExceptionWrapper* status, ProfilerPlugin* aPlugin, ITransaction* transaction,
			const char* aDescription, ISC_TIMESTAMP_TZ aStartTimestamp);

public:
	void dispose() override
	{
		plugin = nullptr;	// avoid circular reference
		release();
	}

public:
	SINT64 getId() override
	{
		return id;
	}

	unsigned getFlags() override
	{
		return FLAG_AFTER_EVENTS;
	}

	void finish(ThrowStatusExceptionWrapper* status, ISC_TIMESTAMP_TZ timestamp) override;

	void defineStatement(ThrowStatusExceptionWrapper* status, SINT64 statementId, SINT64 parentStatementId,
		const char* type, const char* packageName, const char* routineName, const char* sqlText) override;

	void defineRecordSource(SINT64 statementId, unsigned cursorId, unsigned recSourceId,
		const char* accessPath, unsigned parentRecordSourceId) override;

	void onRequestStart(ThrowStatusExceptionWrapper* status, SINT64 requestId, SINT64 statementId,
		SINT64 callerRequestId, ISC_TIMESTAMP_TZ timestamp) override;

	void onRequestFinish(ThrowStatusExceptionWrapper* status, SINT64 requestId, ISC_TIMESTAMP_TZ timestamp) override;

	void beforePsqlLineColumn(SINT64 requestId, unsigned line, unsigned column) override
	{
	}

	void afterPsqlLineColumn(SINT64 requestId, unsigned line, unsigned column, FB_UINT64 runTime) override;

	void beforeRecordSourceOpen(SINT64 requestId, unsigned cursorId, unsigned recSourceId) override
	{
	}

	void afterRecordSourceOpen(SINT64 requestId, unsigned cursorId, unsigned recSourceId, FB_UINT64 runTime) override;

	void beforeRecordSourceGetRecord(SINT64 requestId, unsigned cursorId, unsigned recSourceId) override
	{
	}

	void afterRecordSourceGetRecord(SINT64 requestId, unsigned cursorId, unsigned recSourceId,
		FB_UINT64 runTime) override;

public:
	RefPtr<ProfilerPlugin> plugin;
	NonPooledMap<SINT64, Statement> statements{defaultPool()};
	NonPooledMap<StatementCursorRecSourceKey, RecordSource> recordSources{defaultPool()};
	NonPooledMap<SINT64, Request> requests{defaultPool()};
	SINT64 id;
	bool dirty = true;
	ISC_TIMESTAMP_TZ startTimestamp;
	Nullable<ISC_TIMESTAMP_TZ> finishTimestamp;
	string description{defaultPool()};
};

class ProfilerPlugin final : public StdPlugin<IProfilerPluginImpl<ProfilerPlugin, ThrowStatusExceptionWrapper>>
{
public:
	explicit ProfilerPlugin(IPluginConfig*)
	{
	}

	void init(ThrowStatusExceptionWrapper* status, IAttachment* attachment, ITransaction* transaction) override;

	IProfilerSession* startSession(ThrowStatusExceptionWrapper* status, ITransaction* transaction,
		const char* description, ISC_TIMESTAMP_TZ timestamp) override;

	void flush(ThrowStatusExceptionWrapper* status, ITransaction* transaction) override;

private:
	void createMetadata(ThrowStatusExceptionWrapper* status, RefPtr<IAttachment> attachment,
		RefPtr<ITransaction> transaction);

	void loadMetadata(ThrowStatusExceptionWrapper* status);

public:
	RefPtr<IAttachment> userAttachment;
	ObjectsArray<RefPtr<Session>> sessions{getPool()};
};

//--------------------------------------

void ProfilerPlugin::init(ThrowStatusExceptionWrapper* status, IAttachment* attachment, ITransaction* transaction)
{
	userAttachment = attachment;

	constexpr auto sql = R"""(
		select exists(
		           select true
		               from rdb$roles
		               where rdb$role_name = 'PLG$PROFILER'
		       ) metadata_created,
		       rdb$get_context('SYSTEM', 'DB_NAME') db_name,
		       (select rdb$owner_name
		            from rdb$relations
		            where rdb$relation_name = 'RDB$DATABASE'
		       ) owner_name,
		       current_role,
		       rdb$role_in_use('PLG$PROFILER') role_in_use
		    from rdb$database
	)""";

	FB_MESSAGE(message, ThrowStatusExceptionWrapper,
		(FB_BOOLEAN, metadataCreated)
		(FB_INTL_VARCHAR(MAXPATHLEN * 4, CS_METADATA), dbName)
		(FB_INTL_VARCHAR(MAX_SQL_IDENTIFIER_LEN, CS_METADATA), ownerName)
		(FB_INTL_VARCHAR(MAX_SQL_IDENTIFIER_LEN, CS_METADATA), currentRole)
		(FB_BOOLEAN, roleInUse)
	) message(status, MasterInterfacePtr());
	message.clear();

	RefPtr<IAttachment> refAttachment(attachment);
	RefPtr<ITransaction> refTransaction(transaction);
	string currentRole;
	bool roleInUse;

	for (unsigned i = 0; i < 2; ++i)
	{
		auto resultSet = makeNoIncRef(refAttachment->openCursor(status, refTransaction, 0, sql, SQL_DIALECT_CURRENT,
			nullptr, nullptr, message.getMetadata(), nullptr, 0));

		if (resultSet->fetchNext(status, message.getData()) == IStatus::RESULT_NO_DATA)
		{
			fb_assert(false);
			return;
		}

		if (i == 0)
		{
			currentRole = string(message->currentRole.str, message->currentRole.length);
			quote(currentRole);

			roleInUse = message->roleInUse;

			if (message->metadataCreated)
				break;

			auto dispatcher = makeNoIncRef(MasterInterfacePtr()->getDispatcher());
			const auto util = MasterInterfacePtr()->getUtilInterface();

			const auto dbName = string(message->dbName.str, message->dbName.length);

			auto ownerName = string(message->ownerName.str, message->ownerName.length);
			quote(ownerName);

			AutoDispose<IXpbBuilder> dpb(util->getXpbBuilder(status, IXpbBuilder::DPB, nullptr, 0));
			dpb->insertString(status, isc_dpb_user_name, ownerName.c_str());
			dpb->insertTag(status, isc_dpb_utf8_filename);
			dpb->insertInt(status, isc_dpb_no_db_triggers, 1);

			refAttachment = makeNoIncRef(dispatcher->attachDatabase(status, dbName.c_str(),
				dpb->getBufferLength(status), dpb->getBuffer(status)));

			refTransaction = makeNoIncRef(refAttachment->startTransaction(status, 0, nullptr));
		}
	}

	if (!message->metadataCreated)
		createMetadata(status, refAttachment, refTransaction);

	if (!roleInUse)
	{
		// Refresh roles.

		attachment->execute(status, transaction, 0, "set role plg$profiler",
			SQL_DIALECT_CURRENT, nullptr, nullptr, nullptr, nullptr);

		attachment->execute(status, transaction, 0, ("set role " + currentRole).c_str(),
			SQL_DIALECT_CURRENT, nullptr, nullptr, nullptr, nullptr);
	}

	loadMetadata(status);
}

IProfilerSession* ProfilerPlugin::startSession(ThrowStatusExceptionWrapper* status, ITransaction* transaction,
	const char* description, ISC_TIMESTAMP_TZ timestamp)
{
	return FB_NEW Session(status, this, transaction, description, timestamp);
}

void ProfilerPlugin::flush(ThrowStatusExceptionWrapper* status, ITransaction* transaction)
{
	constexpr auto sessionSql = R"""(
		update or insert into fbprof$sessions
		    (session_id, attachment_id, user_name, description, start_timestamp, finish_timestamp)
		    values (?, current_connection, current_user, ?, ?, ?)
		    matching (session_id)
	)""";

	FB_MESSAGE(SessionMessage, ThrowStatusExceptionWrapper,
		(FB_BIGINT, sessionId)
		(FB_INTL_VARCHAR(255 * 4, CS_UTF8), description)
		(FB_TIMESTAMP_TZ, startTimestamp)
		(FB_TIMESTAMP_TZ, finishTimestamp)
	) sessionMessage(status, MasterInterfacePtr());
	sessionMessage.clear();

	constexpr auto statementSql = R"""(
		update or insert into fbprof$statements
		    (session_id, statement_id, parent_statement_id, statement_type, package_name, routine_name, sql_text)
		    values (?, ?, ?, ?, ?, ?, ?)
		    matching (session_id, statement_id)
	)""";

	FB_MESSAGE(StatementMessage, ThrowStatusExceptionWrapper,
		(FB_BIGINT, sessionId)
		(FB_BIGINT, statementId)
		(FB_BIGINT, parentStatementId)
		(FB_INTL_VARCHAR(20 * 4, CS_UTF8), statementType)
		(FB_INTL_VARCHAR(METADATA_IDENTIFIER_CHAR_LEN * 4, CS_UTF8), packageName)
		(FB_INTL_VARCHAR(METADATA_IDENTIFIER_CHAR_LEN * 4, CS_UTF8), routineName)
		(FB_BLOB, sqlText)
	) statementMessage(status, MasterInterfacePtr());
	statementMessage.clear();

	constexpr auto recSrcsSql = R"""(
		update or insert into fbprof$record_sources
		    (session_id, statement_id, cursor_id, record_source_id,
		     parent_record_source_id, access_path)
		    values (?, ?, ?, ?, ?, ?)
		    matching (session_id, statement_id, cursor_id, record_source_id)
	)""";

	FB_MESSAGE(RecSrcsMessage, ThrowStatusExceptionWrapper,
		(FB_BIGINT, sessionId)
		(FB_BIGINT, statementId)
		(FB_INTEGER, cursorId)
		(FB_INTEGER, recordSourceId)
		(FB_BIGINT, parentRecordSourceId)
		(FB_INTL_VARCHAR(1024 * 4, CS_UTF8), accessPath)
	) recSrcsMessage(status, MasterInterfacePtr());
	recSrcsMessage.clear();

	constexpr auto requestSql = R"""(
		update or insert into fbprof$requests
		    (session_id, request_id, statement_id, caller_request_id, start_timestamp, finish_timestamp)
		    values (?, ?, ?, ?, ?, ?)
		    matching (session_id, request_id)
	)""";

	FB_MESSAGE(RequestMessage, ThrowStatusExceptionWrapper,
		(FB_BIGINT, sessionId)
		(FB_BIGINT, requestId)
		(FB_BIGINT, statementId)
		(FB_BIGINT, callerRequestId)
		(FB_TIMESTAMP_TZ, startTimestamp)
		(FB_TIMESTAMP_TZ, finishTimestamp)
	) requestMessage(status, MasterInterfacePtr());
	requestMessage.clear();

	constexpr auto recSrcStatsSql = R"""(
		execute block (
		    session_id type of column fbprof$record_source_stats.session_id = ?,
		    request_id type of column fbprof$record_source_stats.request_id = ?,
		    cursor_id type of column fbprof$record_source_stats.cursor_id = ?,
		    record_source_id type of column fbprof$record_source_stats.record_source_id = ?,
		    statement_id type of column fbprof$record_source_stats.statement_id = ?,
		    open_counter type of column fbprof$record_source_stats.open_counter = ?,
		    open_min_time type of column fbprof$record_source_stats.open_min_time = ?,
		    open_max_time type of column fbprof$record_source_stats.open_max_time = ?,
		    open_total_time type of column fbprof$record_source_stats.open_total_time = ?,
		    fetch_counter type of column fbprof$record_source_stats.fetch_counter = ?,
		    fetch_min_time type of column fbprof$record_source_stats.fetch_min_time = ?,
		    fetch_max_time type of column fbprof$record_source_stats.fetch_max_time = ?,
		    fetch_total_time type of column fbprof$record_source_stats.fetch_total_time = ?
		)
		as
		begin
		    merge into fbprof$record_source_stats
		        using rdb$database on
		            session_id = :session_id and
		            request_id = :request_id and
		            cursor_id = :cursor_id and
		            record_source_id = :record_source_id
		        when not matched then
		            insert (session_id, request_id, cursor_id, record_source_id, statement_id,
		                    open_counter, open_min_time, open_max_time, open_total_time,
		                    fetch_counter, fetch_min_time, fetch_max_time, fetch_total_time)
		                values (:session_id, :request_id, :cursor_id, :record_source_id, :statement_id,
		                        :open_counter, :open_min_time, :open_max_time, :open_total_time,
		                        :fetch_counter, :fetch_min_time, :fetch_max_time, :fetch_total_time)
		        when matched then
		            update set
		                open_counter = open_counter + :open_counter,
		                open_min_time = minvalue(open_min_time, :open_min_time),
		                open_max_time = maxvalue(open_max_time, :open_max_time),
		                open_total_time = open_total_time + :open_total_time,
		                fetch_counter = fetch_counter + :fetch_counter,
		                fetch_min_time = minvalue(fetch_min_time, :fetch_min_time),
		                fetch_max_time = maxvalue(fetch_max_time, :fetch_max_time),
		                fetch_total_time = fetch_total_time + :fetch_total_time;
		end
	)""";

	FB_MESSAGE(RecSrcStatsMessage, ThrowStatusExceptionWrapper,
		(FB_BIGINT, sessionId)
		(FB_BIGINT, requestId)
		(FB_INTEGER, cursorId)
		(FB_INTEGER, recordSourceId)
		(FB_BIGINT, statementId)
		(FB_BIGINT, openCounter)
		(FB_BIGINT, openMinTime)
		(FB_BIGINT, openMaxTime)
		(FB_BIGINT, openTotalTime)
		(FB_BIGINT, fetchCounter)
		(FB_BIGINT, fetchMinTime)
		(FB_BIGINT, fetchMaxTime)
		(FB_BIGINT, fetchTotalTime)
	) recSrcStatsMessage(status, MasterInterfacePtr());
	recSrcStatsMessage.clear();

	constexpr auto psqlStatsSql = R"""(
		execute block (
		    session_id type of column fbprof$psql_stats.session_id = ?,
		    request_id type of column fbprof$psql_stats.request_id = ?,
		    line_num type of column fbprof$psql_stats.line_num = ?,
		    column_num type of column fbprof$psql_stats.column_num = ?,
		    statement_id type of column fbprof$psql_stats.statement_id = ?,
		    counter type of column fbprof$psql_stats.counter = ?,
		    min_time type of column fbprof$psql_stats.min_time = ?,
		    max_time type of column fbprof$psql_stats.max_time = ?,
		    total_time type of column fbprof$psql_stats.total_time = ?
		)
		as
		begin
		    merge into fbprof$psql_stats
		        using rdb$database on
		            session_id = :session_id and
		            request_id = :request_id and
		            line_num = :line_num and
		            column_num = :column_num
		        when not matched then
		            insert (session_id, request_id, line_num, column_num,
		                    statement_id, counter, min_time, max_time, total_time)
		                values (:session_id, :request_id, :line_num, :column_num,
		                        :statement_id, :counter, :min_time, :max_time, :total_time)
		        when matched then
		            update set
		                counter = counter + :counter,
		                min_time = minvalue(min_time, :min_time),
		                max_time = maxvalue(max_time, :max_time),
		                total_time = total_time + :total_time;
		end
	)""";

	FB_MESSAGE(PsqlStatsMessage, ThrowStatusExceptionWrapper,
		(FB_BIGINT, sessionId)
		(FB_BIGINT, requestId)
		(FB_INTEGER, lineNum)
		(FB_INTEGER, columnNum)
		(FB_BIGINT, statementId)
		(FB_BIGINT, counter)
		(FB_BIGINT, minTime)
		(FB_BIGINT, maxTime)
		(FB_BIGINT, totalTime)
	) psqlStatsMessage(status, MasterInterfacePtr());
	psqlStatsMessage.clear();

	auto sessionStmt = makeNoIncRef(userAttachment->prepare(status, transaction, 0, sessionSql, SQL_DIALECT_CURRENT, 0));
	auto statementStmt = makeNoIncRef(userAttachment->prepare(
		status, transaction, 0, statementSql, SQL_DIALECT_CURRENT, 0));
	auto recSrcsStmt = makeNoIncRef(userAttachment->prepare(
		status, transaction, 0, recSrcsSql, SQL_DIALECT_CURRENT, 0));
	auto requestBatch = makeNoIncRef(userAttachment->createBatch(status, transaction, 0, requestSql, SQL_DIALECT_CURRENT,
		requestMessage.getMetadata(), 0, nullptr));
	auto recSrcStatsBatch = makeNoIncRef(userAttachment->createBatch(
		status, transaction, 0, recSrcStatsSql, SQL_DIALECT_CURRENT, recSrcStatsMessage.getMetadata(), 0, nullptr));
	auto psqlStatsBatch = makeNoIncRef(userAttachment->createBatch(
		status, transaction, 0, psqlStatsSql, SQL_DIALECT_CURRENT, psqlStatsMessage.getMetadata(), 0, nullptr));

	unsigned requestBatchSize = 0;
	unsigned recSrcStatsBatchSize = 0;
	unsigned psqlStatsBatchSize = 0;

	auto executeBatch = [&](IBatch* batch, unsigned& batchSize)
	{
		if (batchSize)
		{
			batchSize = 0;

			if (auto batchCs = batch->execute(status, transaction))
				batchCs->dispose();
		}
	};

	auto executeBatches = [&]()
	{
		executeBatch(requestBatch, requestBatchSize);
		executeBatch(recSrcStatsBatch, recSrcStatsBatchSize);
		executeBatch(psqlStatsBatch, psqlStatsBatchSize);
	};

	auto addBatch = [&](IBatch* batch, unsigned& batchSize, const auto& message)
	{
		batch->add(status, 1, message.getData());

		if (++batchSize == 1000)
			executeBatches();
	};

	for (unsigned sessionIdx = 0; sessionIdx < sessions.getCount(); )
	{
		auto& session = sessions[sessionIdx];

		if (session->dirty)
		{
			sessionMessage->sessionIdNull = FB_FALSE;
			sessionMessage->sessionId = session->getId();

			sessionMessage->descriptionNull = session->description.isEmpty();
			sessionMessage->description.set(session->description.c_str());

			sessionMessage->startTimestampNull = FB_FALSE;
			sessionMessage->startTimestamp = session->startTimestamp;

			sessionMessage->finishTimestampNull = session->finishTimestamp.isUnknown();
			sessionMessage->finishTimestamp = session->finishTimestamp.value;

			sessionStmt->execute(status, transaction, sessionMessage.getMetadata(),
				sessionMessage.getData(), nullptr, nullptr);

			session->dirty = false;
		}

		RightPooledMap<unsigned, Array<const NonPooledPair<SINT64, Statement>*>> statementsByLevel;

		for (auto& statementIt : session->statements)
		{
			auto& profileStatement = statementIt.second;

			if (auto currentStatement = &profileStatement; currentStatement->level == 0)
			{
				Stack<Statement*> stack;

				while (currentStatement && currentStatement->level == 0)
				{
					stack.push(currentStatement);
					currentStatement = session->statements.get(currentStatement->parentStatementId);
				}

				unsigned level = currentStatement ? currentStatement->level : 0;

				while (stack.hasData())
					stack.pop()->level = ++level;
			}

			auto levelArray = statementsByLevel.getOrPut(profileStatement.level);
			levelArray->add(&statementIt);
		}

		for (auto& levelIt : statementsByLevel)
		{
			for (auto statementIt : levelIt.second)
			{
				auto profileStatementId = statementIt->first;
				auto& profileStatement = statementIt->second;

				statementMessage->sessionIdNull = FB_FALSE;
				statementMessage->sessionId = session->getId();

				statementMessage->statementIdNull = FB_FALSE;
				statementMessage->statementId = profileStatementId;

				statementMessage->parentStatementIdNull = profileStatement.parentStatementId == 0 ? FB_TRUE : FB_FALSE;
				statementMessage->parentStatementId = profileStatement.parentStatementId;

				statementMessage->statementTypeNull = FB_FALSE;
				statementMessage->statementType.set(profileStatement.type.c_str());

				statementMessage->packageNameNull = profileStatement.packageName.isEmpty();
				statementMessage->packageName.set(profileStatement.packageName.c_str());

				statementMessage->routineNameNull = profileStatement.routineName.isEmpty();
				statementMessage->routineName.set(profileStatement.routineName.c_str());

				statementMessage->sqlTextNull = profileStatement.sqlText.isEmpty();

				if (profileStatement.sqlText.hasData())
				{
					auto blob = makeNoIncRef(userAttachment->createBlob(
						status, transaction, &statementMessage->sqlText, 0, nullptr));
					blob->putSegment(status, profileStatement.sqlText.length(), profileStatement.sqlText.c_str());
					blob->close(status);
					blob.clear();
				}

				statementStmt->execute(status, transaction, statementMessage.getMetadata(),
					statementMessage.getData(), nullptr, nullptr);
			}
		}

		for (const auto& recSourceIt : session->recordSources)
		{
			const auto statementId = recSourceIt.first.first.first;
			const auto cursorId = recSourceIt.first.first.second;
			const auto recSourceId = recSourceIt.first.second;
			const auto& recSrc = recSourceIt.second;

			recSrcsMessage->sessionIdNull = FB_FALSE;
			recSrcsMessage->sessionId = session->getId();

			recSrcsMessage->statementIdNull = FB_FALSE;
			recSrcsMessage->statementId = statementId;

			recSrcsMessage->cursorIdNull = FB_FALSE;
			recSrcsMessage->cursorId = cursorId;

			recSrcsMessage->recordSourceIdNull = FB_FALSE;
			recSrcsMessage->recordSourceId = recSourceId;

			recSrcsMessage->parentRecordSourceIdNull = !recSrc.parentId.specified;
			recSrcsMessage->parentRecordSourceId = recSrc.parentId.value;

			recSrcsMessage->accessPathNull = FB_FALSE;
			recSrcsMessage->accessPath.set(recSrc.accessPath.c_str());

			recSrcsStmt->execute(status, transaction, recSrcsMessage.getMetadata(),
				recSrcsMessage.getData(), nullptr, nullptr);
		}

		RightPooledMap<unsigned, Array<NonPooledPair<SINT64, Request>*>> requestsByLevel;
		Array<SINT64> finishedRequests;

		for (auto& requestIt : session->requests)
		{
			auto& profileRequest = requestIt.second;

			if (auto currentRequest = &profileRequest; currentRequest->level == 0)
			{
				Stack<Request*> stack;

				while (currentRequest && currentRequest->level == 0)
				{
					stack.push(currentRequest);
					currentRequest = session->requests.get(currentRequest->callerRequestId);
				}

				unsigned level = currentRequest ? currentRequest->level : 0;

				while (stack.hasData())
					stack.pop()->level = ++level;
			}

			auto levelArray = requestsByLevel.getOrPut(profileRequest.level);
			levelArray->add(&requestIt);
		}

		for (auto& levelIt : requestsByLevel)
		{
			for (auto requestIt : levelIt.second)
			{
				auto profileRequestId = requestIt->first;
				auto& profileRequest = requestIt->second;

				if (profileRequest.dirty)
				{
					requestMessage->sessionIdNull = FB_FALSE;
					requestMessage->sessionId = session->getId();

					requestMessage->requestIdNull = FB_FALSE;
					requestMessage->requestId = profileRequestId;

					requestMessage->statementIdNull = FB_FALSE;
					requestMessage->statementId = profileRequest.statementId;

					requestMessage->callerRequestIdNull = profileRequest.callerRequestId == 0 ? FB_TRUE : FB_FALSE;
					requestMessage->callerRequestId = profileRequest.callerRequestId;

					requestMessage->startTimestampNull = FB_FALSE;
					requestMessage->startTimestamp = profileRequest.startTimestamp;

					requestMessage->finishTimestampNull = profileRequest.finishTimestamp.isUnknown();
					requestMessage->finishTimestamp = profileRequest.finishTimestamp.value;

					addBatch(requestBatch, requestBatchSize, requestMessage);

					if (profileRequest.finishTimestamp.isAssigned())
						finishedRequests.add(profileRequestId);

					profileRequest.dirty = false;
				}

				for (const auto& statsIt : profileRequest.recordSourcesStats)
				{
					const auto& cursorRecSource = statsIt.first;
					const auto& stats = statsIt.second;

					recSrcStatsMessage->sessionIdNull = FB_FALSE;
					recSrcStatsMessage->sessionId = session->getId();

					recSrcStatsMessage->requestIdNull = FB_FALSE;
					recSrcStatsMessage->requestId = profileRequestId;

					recSrcStatsMessage->cursorIdNull = FB_FALSE;
					recSrcStatsMessage->cursorId = cursorRecSource.first;

					recSrcStatsMessage->recordSourceIdNull = FB_FALSE;
					recSrcStatsMessage->recordSourceId = cursorRecSource.second;

					recSrcStatsMessage->statementIdNull = FB_FALSE;
					recSrcStatsMessage->statementId = profileRequest.statementId;

					recSrcStatsMessage->openCounterNull = FB_FALSE;
					recSrcStatsMessage->openCounter = stats.openStats.counter;

					recSrcStatsMessage->openMinTimeNull = FB_FALSE;
					recSrcStatsMessage->openMinTime = stats.openStats.minTime;

					recSrcStatsMessage->openMaxTimeNull = FB_FALSE;
					recSrcStatsMessage->openMaxTime = stats.openStats.maxTime;

					recSrcStatsMessage->openTotalTimeNull = FB_FALSE;
					recSrcStatsMessage->openTotalTime = stats.openStats.totalTime;

					recSrcStatsMessage->fetchCounterNull = FB_FALSE;
					recSrcStatsMessage->fetchCounter = stats.fetchStats.counter;

					recSrcStatsMessage->fetchMinTimeNull = FB_FALSE;
					recSrcStatsMessage->fetchMinTime = stats.fetchStats.minTime;

					recSrcStatsMessage->fetchMaxTimeNull = FB_FALSE;
					recSrcStatsMessage->fetchMaxTime = stats.fetchStats.maxTime;

					recSrcStatsMessage->fetchTotalTimeNull = FB_FALSE;
					recSrcStatsMessage->fetchTotalTime = stats.fetchStats.totalTime;

					addBatch(recSrcStatsBatch, recSrcStatsBatchSize, recSrcStatsMessage);
				}

				profileRequest.recordSourcesStats.clear();

				for (const auto& statsIt : profileRequest.psqlStats)
				{
					const auto& lineColumn = statsIt.first;

					psqlStatsMessage->sessionIdNull = FB_FALSE;
					psqlStatsMessage->sessionId = session->getId();

					psqlStatsMessage->requestIdNull = FB_FALSE;
					psqlStatsMessage->requestId = profileRequestId;

					psqlStatsMessage->lineNumNull = FB_FALSE;
					psqlStatsMessage->lineNum = lineColumn.first;

					psqlStatsMessage->columnNumNull = FB_FALSE;
					psqlStatsMessage->columnNum = lineColumn.second;

					psqlStatsMessage->statementIdNull = FB_FALSE;
					psqlStatsMessage->statementId = profileRequest.statementId;

					psqlStatsMessage->counterNull = FB_FALSE;
					psqlStatsMessage->counter = statsIt.second.counter;

					psqlStatsMessage->minTimeNull = FB_FALSE;
					psqlStatsMessage->minTime = statsIt.second.minTime;

					psqlStatsMessage->maxTimeNull = FB_FALSE;
					psqlStatsMessage->maxTime = statsIt.second.maxTime;

					psqlStatsMessage->totalTimeNull = FB_FALSE;
					psqlStatsMessage->totalTime = statsIt.second.totalTime;

					addBatch(psqlStatsBatch, psqlStatsBatchSize, psqlStatsMessage);
				}

				profileRequest.psqlStats.clear();
			}
		}

		if (session->finishTimestamp.isUnknown())
		{
			session->statements.clear();
			session->recordSources.clear();

			for (const auto requestId : finishedRequests)
				session->requests.remove(requestId);

			++sessionIdx;
		}
		else
			sessions.remove(sessionIdx);
	}

	executeBatches();
}

void ProfilerPlugin::createMetadata(ThrowStatusExceptionWrapper* status, RefPtr<IAttachment> attachment,
	RefPtr<ITransaction> transaction)
{
	constexpr const char* createSqlStaments[] = {
		"create role plg$profiler",

		"grant default plg$profiler to public",

		"create sequence fbprof$session_id",

		"grant usage on sequence fbprof$session_id to plg$profiler",

		R"""(
		create table fbprof$sessions (
		    session_id bigint not null
		        constraint fbprof$sessions_pk
		            primary key
		            using index fbprof$sessions_session,
		    attachment_id bigint not null,
		    user_name char(63) character set utf8 not null,
		    description varchar(255) character set utf8,
		    start_timestamp timestamp with time zone not null,
		    finish_timestamp timestamp with time zone
		))""",

		"grant select, update, insert, delete on table fbprof$sessions to plg$profiler",

		R"""(
		create table fbprof$statements (
		    session_id bigint not null
		        constraint fbprof$statements_session_fk
		            references fbprof$sessions
		            on delete cascade
		            using index fbprof$statements_session,
		    statement_id bigint not null,
		    parent_statement_id bigint,
		    statement_type varchar(20) character set utf8 not null,
		    package_name char(63) character set utf8,
		    routine_name char(63) character set utf8,
		    sql_text blob sub_type text character set utf8,
		    constraint fbprof$statements_pk
		        primary key (session_id, statement_id)
		        using index fbprof$statements_session_statement,
		    constraint fbprof$statements_parent_statement_fk
		        foreign key (session_id, parent_statement_id) references fbprof$statements (session_id, statement_id)
		        on delete cascade
		        using index fbprof$statements_parent_statement
		))""",

		"grant select, update, insert, delete on table fbprof$statements to plg$profiler",

		R"""(
		create table fbprof$record_sources (
		    session_id bigint not null
		        constraint fbprof$record_sources_session_fk
		            references fbprof$sessions
		            on delete cascade
		            using index fbprof$record_sources_session,
		    statement_id bigint not null,
		    cursor_id bigint not null,
		    record_source_id bigint not null,
		    parent_record_source_id bigint,
		    access_path varchar(1024) character set utf8 not null,
		    constraint fbprof$record_sources_pk
		        primary key (session_id, statement_id, cursor_id, record_source_id)
		        using index fbprof$record_sources_session_statement_cursor_recsource,
		    constraint fbprof$record_sources_statement_fk
		        foreign key (session_id, statement_id) references fbprof$statements
		        on delete cascade
		        using index fbprof$record_sources_session_statement,
		    constraint fbprof$record_sources_parent_record_source_fk
		        foreign key (session_id, statement_id, cursor_id, parent_record_source_id)
		        references fbprof$record_sources (session_id, statement_id, cursor_id, record_source_id)
		        on delete cascade
		        using index fbprof$record_sources_session_statement_cursor_parent_rec_src
		))""",

		"grant select, update, insert, delete on table fbprof$record_sources to plg$profiler",

		R"""(
		create table fbprof$requests (
		    session_id bigint not null
		        constraint fbprof$requests_session_fk
		            references fbprof$sessions
		            on delete cascade
		            using index fbprof$requests_session,
		    request_id bigint not null,
		    statement_id bigint not null,
		    caller_request_id bigint,
		    start_timestamp timestamp with time zone not null,
		    finish_timestamp timestamp with time zone,
		    constraint fbprof$requests_pk
		        primary key (session_id, request_id)
		        using index fbprof$requests_session_request,
		    constraint fbprof$requests_statement_fk
		        foreign key (session_id, statement_id) references fbprof$statements
		        on delete cascade
		        using index fbprof$requests_session_statement,
		    constraint fbprof$requests_caller_request_fk
		        foreign key (session_id, caller_request_id) references fbprof$requests (session_id, request_id)
		        on delete cascade
		        using index fbprof$requests_caller_request
		))""",

		"grant select, update, insert, delete on table fbprof$requests to plg$profiler",

		R"""(
		create table fbprof$psql_stats (
		    session_id bigint not null
		        constraint fbprof$psql_stats_session_fk
		            references fbprof$sessions
		            on delete cascade
		            using index fbprof$psql_stats_session,
		    request_id bigint not null,
		    line_num integer not null,
		    column_num integer not null,
		    statement_id bigint not null,
		    counter bigint not null,
		    min_time bigint not null,
		    max_time bigint not null,
		    total_time bigint not null,
		    constraint fbprof$psql_stats_pk
		        primary key (session_id, request_id, line_num, column_num)
		        using index fbprof$psql_stats_session_request_line_column,
		    constraint fbprof$psql_stats_request_fk
		        foreign key (session_id, request_id) references fbprof$requests
		        on delete cascade
		        using index fbprof$psql_stats_session_request,
		    constraint fbprof$psql_stats_statement_fk
		        foreign key (session_id, statement_id) references fbprof$statements
		        on delete cascade
		        using index fbprof$psql_stats_session_statement
		))""",

		"grant select, update, insert, delete on table fbprof$psql_stats to plg$profiler",

		R"""(
		create table fbprof$record_source_stats (
		    session_id bigint not null
		        constraint fbprof$record_source_stats_session_fk
		            references fbprof$sessions
		            on delete cascade
		            using index fbprof$record_source_stats_session_id,
		    request_id bigint not null,
		    cursor_id bigint not null,
		    record_source_id bigint not null,
		    statement_id bigint not null,
		    open_counter bigint not null,
		    open_min_time bigint not null,
		    open_max_time bigint not null,
		    open_total_time bigint not null,
		    fetch_counter bigint not null,
		    fetch_min_time bigint not null,
		    fetch_max_time bigint not null,
		    fetch_total_time bigint not null,
		    constraint fbprof$record_source_stats_pk
		        primary key (session_id, request_id, cursor_id, record_source_id)
		        using index fbprof$record_source_stats_session_request_cursor_recsource,
		    constraint fbprof$record_source_stats_request_fk
		        foreign key (session_id, request_id) references fbprof$requests
		        on delete cascade
		        using index fbprof$record_source_stats_session_request,
		    constraint fbprof$record_source_stats_statement_fk
		        foreign key (session_id, statement_id) references fbprof$statements
		        on delete cascade
		        using index fbprof$record_source_stats_session_statement,
		    constraint fbprof$record_source_stats_record_source_fk
		        foreign key (session_id, statement_id, cursor_id, record_source_id) references fbprof$record_sources
		        on delete cascade
		        using index fbprof$record_source_stats_statement_cursor_record_source
		))""",

		"grant select, update, insert, delete on table fbprof$record_source_stats to plg$profiler",

		R"""(
		create view fbprof$psql_stats_view
		as
		select pstat.session_id,
		       pstat.statement_id,
		       sta.statement_type,
		       sta.package_name,
		       sta.routine_name,
		       sta.parent_statement_id,
		       sta_parent.statement_type parent_statement_type,
		       sta_parent.routine_name parent_routine_name,
		       (select sql_text
		          from fbprof$statements
		          where session_id = pstat.session_id and
		                statement_id = coalesce(sta.parent_statement_id, pstat.statement_id)
		       ) sql_text,
		       pstat.line_num,
		       pstat.column_num,
		       sum(pstat.counter) counter,
		       min(pstat.min_time) min_time,
		       max(pstat.max_time) max_time,
		       sum(pstat.total_time) total_time,
		       sum(pstat.total_time) / nullif(sum(pstat.counter), 0) avg_time
		  from fbprof$psql_stats pstat
		  join fbprof$statements sta
		    on sta.session_id = pstat.session_id and
		       sta.statement_id = pstat.statement_id
		  left join fbprof$statements sta_parent
		    on sta_parent.session_id = sta.session_id and
		       sta_parent.statement_id = sta.parent_statement_id
		  group by pstat.session_id,
		           pstat.statement_id,
		           sta.statement_type,
		           sta.package_name,
		           sta.routine_name,
		           sta.parent_statement_id,
		           sta_parent.statement_type,
		           sta_parent.routine_name,
		           pstat.line_num,
		           pstat.column_num
		  order by sum(pstat.total_time) desc
		)""",

		"grant select on table fbprof$psql_stats_view to plg$profiler",

		R"""(
		create view fbprof$record_source_stats_view
		as
		select rstat.session_id,
		       rstat.statement_id,
		       sta.statement_type,
		       sta.package_name,
		       sta.routine_name,
		       sta.parent_statement_id,
		       sta_parent.statement_type parent_statement_type,
		       sta_parent.routine_name parent_routine_name,
		       (select sql_text
		          from fbprof$statements
		          where session_id = rstat.session_id and
		                statement_id = coalesce(sta.parent_statement_id, rstat.statement_id)
		       ) sql_text,
		       rstat.cursor_id,
		       rstat.record_source_id,
		       recsrc.parent_record_source_id,
		       recsrc.access_path,
		       sum(rstat.open_counter) open_counter,
		       min(rstat.open_min_time) open_min_time,
		       max(rstat.open_max_time) open_max_time,
		       sum(rstat.open_total_time) open_total_time,
		       sum(rstat.open_total_time) / nullif(sum(rstat.open_counter), 0) open_avg_time,
		       sum(rstat.fetch_counter) fetch_counter,
		       min(rstat.fetch_min_time) fetch_min_time,
		       max(rstat.fetch_max_time) fetch_max_time,
		       sum(rstat.fetch_total_time) fetch_total_time,
		       sum(rstat.fetch_total_time) / nullif(sum(rstat.fetch_counter), 0) fetch_avg_time,
		       coalesce(sum(rstat.open_total_time), 0) + coalesce(sum(rstat.fetch_total_time), 0) open_fetch_total_time
		  from fbprof$record_source_stats rstat
		  join fbprof$record_sources recsrc
		    on recsrc.session_id = rstat.session_id and
		       recsrc.statement_id = rstat.statement_id and
		       recsrc.cursor_id = rstat.cursor_id and
		       recsrc.record_source_id = rstat.record_source_id
		  join fbprof$statements sta
		    on sta.session_id = rstat.session_id and
		       sta.statement_id = rstat.statement_id
		  left join fbprof$statements sta_parent
		    on sta_parent.session_id = sta.session_id and
		       sta_parent.statement_id = sta.parent_statement_id
		  group by rstat.session_id,
		           rstat.statement_id,
		           sta.statement_type,
		           sta.package_name,
		           sta.routine_name,
		           sta.parent_statement_id,
		           sta_parent.statement_type,
		           sta_parent.routine_name,
		           rstat.cursor_id,
		           rstat.record_source_id,
		           recsrc.parent_record_source_id,
		           recsrc.access_path
		  order by coalesce(sum(rstat.open_total_time), 0) + coalesce(sum(rstat.fetch_total_time), 0) desc
		)""",

		"grant select on table fbprof$record_source_stats_view to plg$profiler"
	};

	for (auto createSql : createSqlStaments)
	{
		attachment->execute(status, transaction, 0, createSql, SQL_DIALECT_CURRENT,
			nullptr, nullptr, nullptr, nullptr);
	}

	transaction->commit(status);
	transaction.clear();
}

// Load objects in engine caches so they can be used in the user's transaction.
void ProfilerPlugin::loadMetadata(ThrowStatusExceptionWrapper* status)
{
	constexpr auto loadObjectsSql =
		R"""(
		select *
		    from fbprof$sessions
		    cross join fbprof$statements
		    cross join fbprof$record_sources
		    cross join fbprof$requests
		    cross join fbprof$psql_stats
		    cross join fbprof$record_source_stats
		    where next value for fbprof$session_id = 0
		)""";

	auto transaction = makeNoIncRef(userAttachment->startTransaction(status, 0, nullptr));

	makeNoIncRef(userAttachment->prepare(status, transaction, 0, loadObjectsSql, SQL_DIALECT_CURRENT, 0));

	transaction->commit(status);
	transaction.clear();
}

//--------------------------------------

Session::Session(ThrowStatusExceptionWrapper* status, ProfilerPlugin* aPlugin, ITransaction* transaction,
		const char* aDescription, ISC_TIMESTAMP_TZ aStartTimestamp)
	: plugin(aPlugin),
	  startTimestamp(aStartTimestamp),
	  description(defaultPool(), aDescription)
{
	FB_MESSAGE(SequenceMessage, ThrowStatusExceptionWrapper,
		(FB_BIGINT, value)
	) sequenceMessage(status, MasterInterfacePtr());
	sequenceMessage.clear();

	constexpr auto sequenceSql = "select next value for fbprof$session_id from rdb$database";

	auto resultSet = makeNoIncRef(plugin->userAttachment->openCursor(status, transaction, 0, sequenceSql,
		SQL_DIALECT_CURRENT,
		nullptr, nullptr, sequenceMessage.getMetadata(), nullptr, 0));

	resultSet->fetchNext(status, sequenceMessage.getData());
	id = sequenceMessage->value;

	plugin->sessions.add(makeRef(this));

	addRef();
}

void Session::finish(ThrowStatusExceptionWrapper* status, ISC_TIMESTAMP_TZ timestamp)
{
	dirty = true;
	finishTimestamp = timestamp;
}

void Session::defineStatement(ThrowStatusExceptionWrapper* status, SINT64 statementId, SINT64 parentStatementId,
	const char* type, const char* packageName, const char* routineName, const char* sqlText)
{
	auto statement = statements.put(statementId);
	fb_assert(statement);

	if (!statement)
		return;

	statement->type = type;
	statement->packageName = packageName;
	statement->routineName = routineName;
	statement->parentStatementId = parentStatementId;
	statement->sqlText = sqlText;
}

void Session::defineRecordSource(SINT64 statementId, unsigned cursorId, unsigned recSourceId,
	const char* accessPath, unsigned parentRecordSourceId)
{
	const auto recSource = recordSources.put({{statementId, cursorId}, recSourceId});
	fb_assert(recSource);

	if (!recSource)
		return;

	recSource->accessPath = accessPath;

	constexpr unsigned MAX_ACCESS_PATH_CHAR_LEN = 1024;

	if (unsigned len = recSource->accessPath.length(); len > MAX_ACCESS_PATH_CHAR_LEN)
	{
		auto str = recSource->accessPath.c_str();
		unsigned charLen = 0;
		unsigned pos = 0;
		unsigned truncPos = 0;

		while (pos < len && charLen <= MAX_ACCESS_PATH_CHAR_LEN)
		{
			UChar32 c;
			U8_NEXT_UNSAFE(str, pos, c);
			++charLen;

			if (charLen == MAX_ACCESS_PATH_CHAR_LEN - 3)
				truncPos = pos;
		}

		if (charLen > MAX_ACCESS_PATH_CHAR_LEN)
		{
			recSource->accessPath.resize(truncPos);
			recSource->accessPath += "...";
		}
	}

	if (parentRecordSourceId)
		recSource->parentId = parentRecordSourceId;
}

void Session::onRequestStart(ThrowStatusExceptionWrapper* status, SINT64 requestId, SINT64 statementId,
	SINT64 callerRequestId, ISC_TIMESTAMP_TZ timestamp)
{
	auto request = requests.put(requestId);
	///fb_assert(!request);

	if (!request)
		return;

	request->statementId = statementId;
	request->callerRequestId = callerRequestId;
	request->startTimestamp = timestamp;
}

void Session::onRequestFinish(ThrowStatusExceptionWrapper* status, SINT64 requestId, ISC_TIMESTAMP_TZ timestamp)
{
	if (auto request = requests.get(requestId))
	{
		request->dirty = true;
		request->finishTimestamp = timestamp;
	}
}

void Session::afterPsqlLineColumn(SINT64 requestId, unsigned line, unsigned column, FB_UINT64 runTime)
{
	if (auto request = requests.get(requestId))
	{
		const auto profileStats = request->psqlStats.getOrPut({line, column});
		profileStats->hit(runTime);
	}
}

void Session::afterRecordSourceOpen(SINT64 requestId, unsigned cursorId, unsigned recSourceId, FB_UINT64 runTime)
{
	if (auto request = requests.get(requestId))
	{
		auto stats = request->recordSourcesStats.getOrPut({cursorId, recSourceId});
		stats->openStats.hit(runTime);
	}
}

void Session::afterRecordSourceGetRecord(SINT64 requestId, unsigned cursorId, unsigned recSourceId, FB_UINT64 runTime)
{
	if (auto request = requests.get(requestId))
	{
		auto stats = request->recordSourcesStats.getOrPut({cursorId, recSourceId});
		stats->fetchStats.hit(runTime);
	}
}

//--------------------------------------

SimpleFactory<ProfilerPlugin> factory;

} // anonymous namespace

extern "C" void FB_EXPORTED FB_PLUGIN_ENTRY_POINT(IMaster* master)
{
	CachedMasterInterface::set(master);
	PluginManagerInterfacePtr()->registerPluginFactory(IPluginManager::TYPE_PROFILER, "Default_Profiler", &factory);
	getUnloadDetector()->registerMe();
}
