#include <hiredis/hiredis.h>
#include <system_error>

#include "common/table.h"
#include "common/logger.h"
#include "common/redisreply.h"
#include "common/rediscommand.h"
#include "common/redisapi.h"
#include "common/json.hpp"

using namespace std;
using namespace swss;
using json = nlohmann::json;

Table::Table(DBConnector *db, string tableName) : RedisTransactioner(db), TableBase(tableName)
{
}

bool Table::get(string key, vector<FieldValueTuple> &values)
{
    string hgetall_key("HGETALL ");
    hgetall_key += getKeyName(key);

    RedisReply r(m_db, hgetall_key, REDIS_REPLY_ARRAY);
    redisReply *reply = r.getContext();
    values.clear();

    if (!reply->elements)
        return false;

    if (reply->elements & 1)
        throw system_error(make_error_code(errc::address_not_available),
                           "Unable to connect netlink socket");

    for (unsigned int i = 0; i < reply->elements; i += 2)
        values.push_back(make_tuple(reply->element[i]->str,
                                    reply->element[i + 1]->str));

    return true;
}

void Table::set(string key, vector<FieldValueTuple> &values,
                string /*op*/, string /*prefix*/)
{
    if (values.size() == 0)
        return;

    RedisCommand cmd;
    cmd.formatHMSET(getKeyName(key), values);

    RedisReply r(m_db, cmd, REDIS_REPLY_STATUS);

    r.checkStatusOK();
}

void Table::del(string key, string /* op */, string /*prefix*/)
{
    RedisReply r(m_db, string("DEL ") + getKeyName(key), REDIS_REPLY_INTEGER);
}

void TableEntryEnumerable::getTableContent(vector<KeyOpFieldsValuesTuple> &tuples)
{
    vector<string> keys;
    getTableKeys(keys);

    tuples.clear();

    for (auto key: keys)
    {
        vector<FieldValueTuple> values;
        string op = "";

        get(key, values);
        tuples.push_back(make_tuple(key, op, values));
    }
}

void Table::getTableKeys(vector<string> &keys)
{
    string keys_cmd("KEYS " + getTableName() + ":*");
    RedisReply r(m_db, keys_cmd, REDIS_REPLY_ARRAY);
    redisReply *reply = r.getContext();
    keys.clear();

    for (unsigned int i = 0; i < reply->elements; i++)
    {
        string key = reply->element[i]->str;
        auto pos = key.find(':');
        keys.push_back(key.substr(pos+1));
    }
}

void Table::dump(TableDump& tableDump)
{
    SWSS_LOG_ENTER();

    // note that this function is not efficient
    // it can take ~100ms for entire asic dump
    // but it's not intended to be efficient
    // since it will not be used many times

    static std::string luaScript =

        "local keys = redis.call(\"keys\", KEYS[1] .. \":*\")\n"
        "local res = {}\n"

        "for i,k in pairs(keys) do\n"
        "   local skeys = redis.call(\"HKEYS\", k)\n"
        "   local sres={}\n"

        "   for j,sk in pairs(skeys) do\n"
        "       sres[sk] = redis.call(\"HGET\", k, sk)\n"
        "   end\n"

        "   res[k] = sres\n"

        "end\n"

        "return cjson.encode(res)\n";

    static std::string sha = loadRedisScript(m_db, luaScript);

    SWSS_LOG_TIMER("getting");

    RedisCommand command;
    command.format("EVALSHA %s 1 %s ''",
            sha.c_str(),
            getTableName().c_str());

    RedisReply r(m_db, command, REDIS_REPLY_STRING);

    auto ctx = r.getContext();

    std::string data = ctx->str;

    json j = json::parse(data);

    size_t tableNameLen = getTableName().length() + 1; // + ":"

    for (json::iterator it = j.begin(); it != j.end(); ++it)
    {
        TableMap map;

        json jj = it.value();

        for (json::iterator itt = jj.begin(); itt != jj.end(); ++itt)
        {
            if (itt.key() == "NULL")
            {
                continue;
            }

            map[itt.key()] = itt.value();
        }

        std::string key = it.key().substr(tableNameLen);

        tableDump[key] = map;
    }
}
