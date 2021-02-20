#include "PostgreSQLReplicaConsumer.h"

#if USE_LIBPQXX
#include "StoragePostgreSQLReplica.h"

#include <Columns/ColumnNullable.h>
#include <Common/hex.h>
#include <DataStreams/copyData.h>
#include <DataStreams/OneBlockInputStream.h>
#include <DataTypes/DataTypeNullable.h>
#include <ext/range.h>
#include <Interpreters/Context.h>
#include <Interpreters/InterpreterInsertQuery.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int UNKNOWN_TABLE;
}

PostgreSQLReplicaConsumer::PostgreSQLReplicaConsumer(
    std::shared_ptr<Context> context_,
    PostgreSQLConnectionPtr connection_,
    const std::string & replication_slot_name_,
    const std::string & publication_name_,
    const std::string & metadata_path,
    const std::string & start_lsn,
    const size_t max_block_size_,
    Storages storages_)
    : log(&Poco::Logger::get("PostgreSQLReaplicaConsumer"))
    , context(context_)
    , replication_slot_name(replication_slot_name_)
    , publication_name(publication_name_)
    , metadata(metadata_path)
    , connection(std::move(connection_))
    , current_lsn(start_lsn)
    , max_block_size(max_block_size_)
    , storages(storages_)
{
    for (const auto & [table_name, storage] : storages)
    {
        buffers.emplace(table_name, BufferData(storage));
    }
}


void PostgreSQLReplicaConsumer::readMetadata()
{
    try
    {
        metadata.readMetadata();

        if (!metadata.lsn().empty())
        {
            auto tx = std::make_shared<pqxx::nontransaction>(*connection->conn());
            final_lsn = metadata.lsn();
            final_lsn = advanceLSN(tx);
            tx->commit();
        }
    }
    catch (...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__);
    }
}


void PostgreSQLReplicaConsumer::insertValue(BufferData & buffer, const std::string & value, size_t column_idx)
{
    const auto & sample = buffer.description.sample_block.getByPosition(column_idx);
    bool is_nullable = buffer.description.types[column_idx].second;

    if (is_nullable)
    {
        ColumnNullable & column_nullable = assert_cast<ColumnNullable &>(*buffer.columns[column_idx]);
        const auto & data_type = assert_cast<const DataTypeNullable &>(*sample.type);

        insertPostgreSQLValue(
                column_nullable.getNestedColumn(), value,
                buffer.description.types[column_idx].first, data_type.getNestedType(), buffer.array_info, column_idx);

        column_nullable.getNullMapData().emplace_back(0);
    }
    else
    {
        insertPostgreSQLValue(
                *buffer.columns[column_idx], value,
                buffer.description.types[column_idx].first, sample.type,
                buffer.array_info, column_idx);
    }
}


void PostgreSQLReplicaConsumer::insertDefaultValue(BufferData & buffer, size_t column_idx)
{
    const auto & sample = buffer.description.sample_block.getByPosition(column_idx);
    insertDefaultPostgreSQLValue(*buffer.columns[column_idx], *sample.column);
}


void PostgreSQLReplicaConsumer::readString(const char * message, size_t & pos, size_t size, String & result)
{
    assert(size > pos + 2);
    char current = unhex2(message + pos);
    pos += 2;
    while (pos < size && current != '\0')
    {
        result += current;
        current = unhex2(message + pos);
        pos += 2;
    }
}


Int32 PostgreSQLReplicaConsumer::readInt32(const char * message, size_t & pos, [[maybe_unused]] size_t size)
{
    assert(size > pos + 8);
    Int32 result = (UInt32(unhex2(message + pos)) << 24)
                | (UInt32(unhex2(message + pos + 2)) << 16)
                | (UInt32(unhex2(message + pos + 4)) << 8)
                | (UInt32(unhex2(message + pos + 6)));
    pos += 8;
    return result;
}


Int16 PostgreSQLReplicaConsumer::readInt16(const char * message, size_t & pos, [[maybe_unused]] size_t size)
{
    assert(size > pos + 4);
    Int16 result = (UInt32(unhex2(message + pos)) << 8)
                | (UInt32(unhex2(message + pos + 2)));
    pos += 4;
    return result;
}


Int8 PostgreSQLReplicaConsumer::readInt8(const char * message, size_t & pos, [[maybe_unused]] size_t size)
{
    assert(size > pos + 2);
    Int8 result = unhex2(message + pos);
    pos += 2;
    return result;
}


Int64 PostgreSQLReplicaConsumer::readInt64(const char * message, size_t & pos, [[maybe_unused]] size_t size)
{
    assert(size > pos + 16);
    Int64 result = (UInt64(unhex4(message + pos)) << 48)
                | (UInt64(unhex4(message + pos + 4)) << 32)
                | (UInt64(unhex4(message + pos + 8)) << 16)
                | (UInt64(unhex4(message + pos + 12)));
    pos += 16;
    return result;
}


void PostgreSQLReplicaConsumer::readTupleData(
        BufferData & buffer, const char * message, size_t & pos, [[maybe_unused]] size_t size, PostgreSQLQuery type, bool old_value)
{
    Int16 num_columns = readInt16(message, pos, size);
    LOG_DEBUG(log, "number of columns {}", num_columns);

    auto proccess_column_value = [&](Int8 identifier, Int16 column_idx)
    {
        char id = identifier;
        LOG_DEBUG(log, "Identifier: {}", id);
        switch (identifier)
        {
            case 'n': /// NULL
            {
                insertDefaultValue(buffer, column_idx);
                break;
            }
            case 't': /// Text formatted value
            {
                Int32 col_len = readInt32(message, pos, size);
                String value;

                for (Int32 i = 0; i < col_len; ++i)
                    value += readInt8(message, pos, size);

                insertValue(buffer, value, column_idx);
                break;
            }
            case 'u': /// Toasted (unchanged) value TODO:!
                break;
        }
    };

    for (int column_idx = 0; column_idx < num_columns; ++column_idx)
        proccess_column_value(readInt8(message, pos, size), column_idx);

    switch (type)
    {
        case PostgreSQLQuery::INSERT:
        {
            buffer.columns[num_columns]->insert(Int8(1));
            buffer.columns[num_columns + 1]->insert(UInt64(metadata.version()));

            break;
        }
        case PostgreSQLQuery::DELETE:
        {
            buffer.columns[num_columns]->insert(Int8(-1));
            buffer.columns[num_columns + 1]->insert(UInt64(metadata.version()));

            break;
        }
        case PostgreSQLQuery::UPDATE:
        {
            if (old_value) /// Only if replica identity is set to full
                buffer.columns[num_columns]->insert(Int8(-1));
            else
                buffer.columns[num_columns]->insert(Int8(1));

            buffer.columns[num_columns + 1]->insert(UInt64(metadata.version()));

            break;
        }
    }
}


/// https://www.postgresql.org/docs/13/protocol-logicalrep-message-formats.html
void PostgreSQLReplicaConsumer::processReplicationMessage(const char * replication_message, size_t size)
{
    /// Skip '\x'
    size_t pos = 2;
    char type = readInt8(replication_message, pos, size);

    LOG_DEBUG(log, "Type of replication message: {}", type);

    switch (type)
    {
        case 'B': // Begin
        {
            readInt64(replication_message, pos, size); /// Int64 transaction end lsn
            readInt64(replication_message, pos, size); /// Int64 transaction commit timestamp
            break;
        }
        case 'C': // Commit
        {
            readInt8(replication_message, pos, size);  /// unused flags
            readInt64(replication_message, pos, size); /// Int64 commit lsn
            readInt64(replication_message, pos, size); /// Int64 transaction end lsn
            readInt64(replication_message, pos, size); /// Int64 transaction commit timestamp

            final_lsn = current_lsn;
            break;
        }
        case 'O': // Origin
            break;
        case 'R': // Relation
        {
            Int32 relation_id = readInt32(replication_message, pos, size);

            String relation_namespace, relation_name;

            readString(replication_message, pos, size, relation_namespace);
            readString(replication_message, pos, size, relation_name);

            table_to_insert = relation_name;
            tables_to_sync.insert(table_to_insert);

            /// TODO: Add replica identity settings to metadata (needed for update)
            Int8 replica_identity = readInt8(replication_message, pos, size);
            Int16 num_columns = readInt16(replication_message, pos, size);

            LOG_DEBUG(log,
                    "INFO: relation id: {}, namespace: {}, relation name: {}, replica identity: {}, columns number: {}",
                    relation_id, relation_namespace, relation_name, replica_identity, num_columns);

            /// Cache table schema data to be able to detect schema changes, because ddl is not
            /// replicated with postgresql logical replication protocol, but some table schema info
            /// is received if it is the first time we received dml message for given relation in current session or
            /// if relation definition has changed since the last relation definition message.
            Int8 key; /// Flags. 0 or 1 (if part of the key). Not needed for now.
            Int32 data_type_id;
            Int32 type_modifier; /// For example, n in varchar(n)

            bool new_relation_definition = false;
            if (relation_id_to_name.find(relation_id) == relation_id_to_name.end())
            {
                relation_id_to_name.emplace(relation_id, relation_name);
                schema_data.emplace(relation_id, SchemaData(num_columns));
                new_relation_definition = true;
            }

            auto & current_schema_data = schema_data.find(relation_id)->second;

            if (current_schema_data.number_of_columns != num_columns)
            {
                markTableAsSkipped(relation_id, relation_name);
                return;
            }

            for (uint16_t i = 0; i < num_columns; ++i)
            {
                String column_name;
                key = readInt8(replication_message, pos, size);
                readString(replication_message, pos, size, column_name);

                data_type_id = readInt32(replication_message, pos, size);
                type_modifier = readInt32(replication_message, pos, size);

                LOG_DEBUG(log,
                        "Key: {}, column name: {}, data type id: {}, type modifier: {}",
                        key, column_name, data_type_id, type_modifier);

                if (new_relation_definition)
                {
                    current_schema_data.column_identifiers.emplace_back(std::make_tuple(data_type_id, type_modifier));
                }
                else
                {
                    if (current_schema_data.column_identifiers[i].first != data_type_id
                            || current_schema_data.column_identifiers[i].second != type_modifier)
                    {
                        markTableAsSkipped(relation_id, relation_name);
                        return;
                    }
                }
            }

            if (storages.find(table_to_insert) == storages.end())
            {
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "Storage for table {} does not exist, but is included in replication stream", table_to_insert);
            }

            [[maybe_unused]] auto buffer_iter = buffers.find(table_to_insert);
            assert(buffer_iter != buffers.end());

            break;
        }
        case 'Y': // Type
            break;
        case 'I': // Insert
        {
            Int32 relation_id = readInt32(replication_message, pos, size);

            if (skip_list.find(relation_id) != skip_list.end())
                break;

            Int8 new_tuple = readInt8(replication_message, pos, size);

            LOG_DEBUG(log, "relationID: {}, newTuple: {}, current insert table: {}", relation_id, new_tuple, table_to_insert);

            auto buffer = buffers.find(table_to_insert);
            if (buffer == buffers.end())
            {
                throw Exception(ErrorCodes::UNKNOWN_TABLE,
                        "Buffer for table {} does not exist", table_to_insert);
            }

            readTupleData(buffer->second, replication_message, pos, size, PostgreSQLQuery::INSERT);
            break;
        }
        case 'U': // Update
        {
            Int32 relation_id = readInt32(replication_message, pos, size);

            if (skip_list.find(relation_id) != skip_list.end())
                break;

            LOG_DEBUG(log, "relationID {}, current insert table {}", relation_id, table_to_insert);

            auto buffer = buffers.find(table_to_insert);
            auto proccess_identifier = [&](Int8 identifier) -> bool
            {
                char id = identifier;
                LOG_DEBUG(log, "Identifier: {}", id);
                bool read_next = true;
                switch (identifier)
                {
                    case 'K': /// TODO:!
                    {
                        /// Only if changed column(s) are part of replica identity index
                        break;
                    }
                    case 'O':
                    {
                        /// Old row. Only of replica identity is set to full.
                        /// (For the case when a table does not have any primary key.)
                        /// TODO: Need to find suitable order_by for nested table (Now it throws if no primary key)
                        readTupleData(buffer->second, replication_message, pos, size, PostgreSQLQuery::UPDATE, true);
                        break;
                    }
                    case 'N':
                    {
                        readTupleData(buffer->second, replication_message, pos, size, PostgreSQLQuery::UPDATE);
                        read_next = false;
                        break;
                    }
                }

                return read_next;
            };

            /// Read either 'K' or 'O'. Never both of them. Also possible not to get both of them.
            bool read_next = proccess_identifier(readInt8(replication_message, pos, size));

            /// 'N'. Always present, but could come in place of 'K' and 'O'.
            if (read_next)
                proccess_identifier(readInt8(replication_message, pos, size));

            break;
        }
        case 'D': // Delete
        {
            Int32 relation_id = readInt32(replication_message, pos, size);

            if (skip_list.find(relation_id) != skip_list.end())
                break;

            Int8 full_replica_identity = readInt8(replication_message, pos, size);

            LOG_DEBUG(log, "relationID: {}, full replica identity: {}", relation_id, full_replica_identity);

            auto buffer = buffers.find(table_to_insert);
            readTupleData(buffer->second, replication_message, pos, size, PostgreSQLQuery::DELETE);
            break;
        }
        case 'T': // Truncate
            break;
        default:
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                    "Unexpected byte1 value {} while parsing replication message", type);
    }
}


void PostgreSQLReplicaConsumer::markTableAsSkipped(Int32 relation_id, const String & relation_name)
{
    skip_list.insert(relation_id);
    auto & buffer = buffers.find(relation_name)->second;
    buffer.columns = buffer.description.sample_block.cloneEmptyColumns();
    LOG_DEBUG(log, "Table {} is skipped temporarily", relation_name);
}


void PostgreSQLReplicaConsumer::syncTables(std::shared_ptr<pqxx::nontransaction> tx)
{
    for (const auto & table_name : tables_to_sync)
    {
        try
        {
            auto & buffer = buffers.find(table_name)->second;
            Block result_rows = buffer.description.sample_block.cloneWithColumns(std::move(buffer.columns));

            if (result_rows.rows())
            {
                metadata.commitMetadata(final_lsn, [&]()
                {
                    auto storage = storages[table_name];

                    auto insert = std::make_shared<ASTInsertQuery>();
                    insert->table_id = storage->getStorageID();
                    insert->columns = buffer.columnsAST;

                    auto insert_context(*context);
                    insert_context.makeQueryContext();
                    insert_context.addQueryFactoriesInfo(Context::QueryLogFactories::Storage, "ReplacingMergeTree");

                    InterpreterInsertQuery interpreter(insert, insert_context, true);
                    auto block_io = interpreter.execute();
                    OneBlockInputStream input(result_rows);

                    assertBlocksHaveEqualStructure(input.getHeader(), block_io.out->getHeader(), "postgresql replica table sync");
                    copyData(input, *block_io.out);

                    auto actual_lsn = advanceLSN(tx);
                    buffer.columns = buffer.description.sample_block.cloneEmptyColumns();

                    return actual_lsn;
                });
            }
        }
        catch (...)
        {
            tryLogCurrentException(__PRETTY_FUNCTION__);
        }
    }

    LOG_DEBUG(log, "Table sync end for {} tables", tables_to_sync.size());
    tables_to_sync.clear();
    tx->commit();
}


String PostgreSQLReplicaConsumer::advanceLSN(std::shared_ptr<pqxx::nontransaction> tx)
{
    std::string query_str = fmt::format("SELECT end_lsn FROM pg_replication_slot_advance('{}', '{}')", replication_slot_name, final_lsn);
    pqxx::result result{tx->exec(query_str)};

    if (!result.empty())
        return result[0][0].as<std::string>();

    return final_lsn;
}


/// Read binary changes from replication slot via copy command.
bool PostgreSQLReplicaConsumer::readFromReplicationSlot()
{
    std::shared_ptr<pqxx::nontransaction> tx;
    bool slot_empty = true;

    try
    {
        tx = std::make_shared<pqxx::nontransaction>(*connection->conn());

        /// Read up to max_block_size rows changes (upto_n_changes parameter). It might return larger number as the limit
        /// is checked only after each transaction block.
        /// Returns less than max_block_changes, if reached end of wal. Sync to table in this case.

        std::string query_str = fmt::format(
                "select lsn, data FROM pg_logical_slot_peek_binary_changes("
                "'{}', NULL, {}, 'publication_names', '{}', 'proto_version', '1')",
                replication_slot_name, max_block_size, publication_name);

        pqxx::stream_from stream(*tx, pqxx::from_query, std::string_view(query_str));

        while (true)
        {
            const std::vector<pqxx::zview> * row{stream.read_row()};

            if (!row)
            {
                stream.complete();

                if (slot_empty)
                {
                    tx->commit();
                    return false;
                }

                break;
            }

            slot_empty = false;
            current_lsn = (*row)[0];

            processReplicationMessage((*row)[1].c_str(), (*row)[1].size());
        }
    }
    catch (const pqxx::sql_error & e)
    {
        /// For now sql replication interface is used and it has the problem that it registers relcache
        /// callbacks on each pg_logical_slot_get_changes and there is no way to invalidate them:
        /// https://github.com/postgres/postgres/blob/master/src/backend/replication/pgoutput/pgoutput.c#L1128
        /// So at some point will get out of limit and then they will be cleaned.
        std::string error_message = e.what();
        if (error_message.find("out of relcache_callback_list slots") == std::string::npos)
            tryLogCurrentException(__PRETTY_FUNCTION__);

        return false;
    }
    catch (const Exception & e)
    {
        if (e.code() == ErrorCodes::UNKNOWN_TABLE)
            throw;

        tryLogCurrentException(__PRETTY_FUNCTION__);
        return false;
    }

    syncTables(tx);
    return true;
}


bool PostgreSQLReplicaConsumer::consume(NameSet & skipped_tables)
{
    if (!readFromReplicationSlot() || !skip_list.empty())
    {
        for (const auto & relation_id : skip_list)
            skipped_tables.insert(relation_id_to_name[relation_id]);

        return false;
    }

    return true;
}

}

#endif
