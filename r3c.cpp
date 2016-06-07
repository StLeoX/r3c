/*
 * Copyright (c) 2016, Jian Yi <eyjian at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "r3c.h"
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define R3C_ASSERT(x) assert(x)

#ifdef __GNUC__
#  define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
#  define UNUSED(x) UNUSED_ ## x
#endif

#define THROW_REDIS_EXCEPTION(errcode, errmsg) throw CRedisException(errcode, errmsg, __FILE__, __LINE__)
#define THROW_REDIS_EXCEPTION_WITH_NODE(errcode, errmsg, node_ip, node_port) throw CRedisException(errcode, errmsg, __FILE__, __LINE__, node_ip, node_port, NULL, NULL)
#define THROW_REDIS_EXCEPTION_WITH_NODE_AND_COMMAND(errcode, errmsg, node_ip, node_port, command, key) throw CRedisException(errcode, errmsg, __FILE__, __LINE__, node_ip, node_port, command, key)
#define REDIS_COMMAND(excepted_reply_type_) redis_command(excepted_reply_type_, command_, command_length_, key_, str1_, str2_, str3_, array_, in_map1_, in_map2_, str6_, str7_, tag_, tag_length_, value_, values_, out_map_, out_vec_, keep_null_, withscores_, which_)
#define PREPARE_REDIS_COMMAND(command, command_length) \
    const char* command_ = command; \
    size_t command_length_ = command_length; \
    const std::string* key_ = &key; \
    const std::string* str1_ = NULL; \
    const std::string* str2_ = NULL; \
    const std::string* str3_ = NULL; \
    const std::vector<std::string>* array_ = NULL; \
    const std::map<std::string, std::string>* in_map1_ = NULL; \
    const std::map<std::string, int64_t>* in_map2_ = NULL; \
    const std::string* str6_ = NULL; \
    const std::string* str7_ = NULL; \
    const char* tag_ = NULL; \
    size_t tag_length_ = 0; \
    std::string* value_ = NULL; \
    std::vector<std::string>* values_ = NULL; \
    std::map<std::string, std::string>* out_map_ = NULL; \
    std::vector<std::pair<std::string, int64_t> >* out_vec_ = NULL; \
    const bool* keep_null_ = NULL; \
    const bool* withscores_ = NULL; \
    std::pair<std::string, uint16_t>* which_ = which

////////////////////////////////////////////////////////////////////////////////
namespace r3c {

enum
{
    CLUSTER_SLOTS = 16384 // number of slots, defined in cluster.h
};

// help to release va_list
struct va_list_helper
{
    va_list_helper(va_list& ap)
        : _ap(ap)
    {
    }

    ~va_list_helper()
    {
        va_end(_ap);
    }

    va_list& _ap;
};

/*
 * Implemented in crc16.c
 */
extern uint16_t crc16(const char *buf, int len);

/* Copy from cluster.c
 *
 * We have 16384 hash slots. The hash slot of a given key is obtained
 * as the least significant 14 bits of the crc16 of the key.
 *
 * However if the key contains the {...} pattern, only the part between
 * { and } is hashed. This may be useful in the future to force certain
 * keys to be in the same node (assuming no resharding is in progress). */
static unsigned int keyHashSlot(const char *key, size_t keylen) {
    size_t s, e; /* start-end indexes of { and } */

    for (s = 0; s < keylen; s++)
        if (key[s] == '{') break;

    /* No '{' ? Hash the whole key. This is the base case. */
    if (s == keylen) return crc16(key,keylen) & 0x3FFF;

    /* '{' found? Check if we have the corresponding '}'. */
    for (e = s+1; e < keylen; e++)
        if (key[e] == '}') break;

    /* No '}' or nothing betweeen {} ? Hash the whole key. */
    if (e == keylen || e == s+1) return crc16(key,keylen) & 0x3FFF;

    /* If we are here there is both a { and a } on its right. Hash
     * what is in the middle between { and }. */
    return crc16(key+s+1,e-s-1) & 0x3FFF; // 0x3FFF == 16383
}

static bool parse_node_string(const std::string node_string, std::string* ip, uint16_t* port)
{
    std::string::size_type colon_pos = node_string.find(':');
    if (colon_pos == std::string::npos)
    {
        return false;
    }
    else
    {
        const std::string port_str = node_string.substr(colon_pos+1);
        *port = atoi(port_str.c_str());
        *ip = node_string.substr(0, colon_pos);
        return true;
    }
}

static void parse_slot_string(const std::string slot_string, int* start_slot, int* end_slot)
{
    std::string::size_type bar_pos = slot_string.find('-');
    if (bar_pos == std::string::npos)
    {
        *start_slot = atoi(slot_string.c_str());
        *end_slot = *start_slot;
    }
    else
    {
        const std::string end_slot_str = slot_string.substr(bar_pos+1);
        *end_slot = atoi(end_slot_str.c_str());
        *start_slot = atoi(slot_string.substr(0, bar_pos).c_str());
    }
}

int split(std::vector<std::string>* tokens, const std::string& source, const std::string& sep, bool skip_sep)
{
    if (sep.empty())
    {
        tokens->push_back(source);
    }
    else if (!source.empty())
    {
        std::string str = source;
        std::string::size_type pos = str.find(sep);

        while (true)
        {
            std::string token = str.substr(0, pos);
            tokens->push_back(token);

            if (std::string::npos == pos)
            {
                break;
            }
            if (skip_sep)
            {
                bool end = false;
                while (0 == strncmp(sep.c_str(), &str[pos+1], sep.size()))
                {
                    pos += sep.size();
                    if (pos >= str.size())
                    {
                        end = true;
                        tokens->push_back(std::string(""));
                        break;
                    }
                }

                if (end)
                    break;
            }

            str = str.substr(pos + sep.size());
            pos = str.find(sep);
        }
    }

    return static_cast<int>(tokens->size());
}

void millisleep(uint32_t millisecond)
{
    struct timespec ts = { millisecond / 1000, (millisecond % 1000) * 1000000 };
    while ((-1 == nanosleep(&ts, &ts)) && (EINTR == errno));
}

unsigned int get_key_slot(const std::string& key)
{
    return keyHashSlot(key.c_str(), key.size());
}

std::string format_string(const char* format, ...)
{
    va_list ap;
    size_t size = getpagesize();
    char* buffer = new char[size];

    while (true)
    {
        va_start(ap, format);
        int expected = vsnprintf(buffer, size, format, ap);

        va_end(ap);
        if (expected > -1 && expected < (int)size)
            break;

        /* Else try again with more space. */
        if (expected > -1)    /* glibc 2.1 */
            size = (size_t)expected + 1; /* precisely what is needed */
        else           /* glibc 2.0 */
            size *= 2;  /* twice the old size */

        delete []buffer;
        buffer = new char[size];
    }

    std::string str = buffer;
    delete []buffer;
    return str;
}

////////////////////////////////////////////////////////////////////////////////
static void null_log_write(const char* UNUSED(format), ...)
{
}

static void r3c_log_write(const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    vprintf(format, ap);
    va_end(ap);
}

static LOG_WRITE g_info_log = r3c_log_write;
static LOG_WRITE g_debug_log = r3c_log_write;

void set_info_log_write(LOG_WRITE info_log)
{
    g_info_log = info_log;
    if (NULL == g_info_log)
        g_info_log = null_log_write;
}

void set_debug_log_write(LOG_WRITE debug_log)
{
    g_debug_log = debug_log;
    if (NULL == g_debug_log)
        g_debug_log = null_log_write;
}

class FreeArgvHelper
{
public:
    FreeArgvHelper(int argc, char* argv[], size_t* argv_len)
        : _argc(argc), _argv(argv), _argv_len(argv_len)
    {
    }

    ~FreeArgvHelper()
    {
        for (int i=0; i<_argc; ++i)
            delete []_argv[i];

        delete []_argv;
        delete []_argv_len;
    }

private:
    int _argc;
    char** _argv;
    size_t* _argv_len;
};

static void print_reply(const char* command, const char* key, unsigned int slot, redisReply* redis_reply, int excepted_reply_type, const std::pair<std::string, uint16_t>& node)
{
    if (REDIS_REPLY_STRING == redis_reply->type)
        (*g_debug_log)("[STRING][%s][SLOT:%u]["PRINT_COLOR_GREEN"KEY:%s"PRINT_COLOR_NONE"][%s:%d]reply: (%d/%d)%s\n", command, slot, key, node.first.c_str(), node.second, redis_reply->type, excepted_reply_type, redis_reply->str);
    else if (REDIS_REPLY_INTEGER == redis_reply->type)
        (*g_debug_log)("[INTEGER][%s][SLOT:%u]["PRINT_COLOR_GREEN"KEY:%s"PRINT_COLOR_NONE"][%s:%d]reply: (%d/%d)%lld\n", command, slot, key, node.first.c_str(), node.second, redis_reply->type, excepted_reply_type, redis_reply->integer);
    else if (REDIS_REPLY_ARRAY == redis_reply->type)
        (*g_debug_log)("[ARRAY][%s][SLOT:%u]["PRINT_COLOR_GREEN"KEY:%s"PRINT_COLOR_NONE"][%s:%d]reply: (%d/%d)%zd\n", command, slot, key, node.first.c_str(), node.second, redis_reply->type, excepted_reply_type, redis_reply->elements);
    else if (REDIS_REPLY_NIL == redis_reply->type)
        (*g_debug_log)("[NIL][%s][SLOT:%u]["PRINT_COLOR_GREEN"KEY:%s"PRINT_COLOR_NONE"][%s:%d]reply: (%d/%d)%s\n", command, slot, key, node.first.c_str(), node.second, redis_reply->type, excepted_reply_type, redis_reply->str);
    else if (REDIS_REPLY_ERROR == redis_reply->type)
        (*g_debug_log)("[ERROR][%s][SLOT:%u]["PRINT_COLOR_GREEN"KEY:%s"PRINT_COLOR_NONE"][%s:%d]reply: (%d/%d)%s\n", command, slot, key, node.first.c_str(), node.second, redis_reply->type, excepted_reply_type, redis_reply->str);
    else if (REDIS_REPLY_STATUS == redis_reply->type)
        (*g_debug_log)("[STATUS][%s][SLOT:%u]["PRINT_COLOR_GREEN"KEY:%s"PRINT_COLOR_NONE"][%s:%d]reply: (%d/%d)%s\n", command, slot, key, node.first.c_str(), node.second, redis_reply->type, excepted_reply_type, redis_reply->str);
    else
        (*g_debug_log)("[->%d][%s][SLOT:%u]["PRINT_COLOR_GREEN"KEY:%s"PRINT_COLOR_NONE"][%s:%d]reply: (%d/%d)%s\n", redis_reply->type, command, slot, key, node.first.c_str(), node.second, redis_reply->type, excepted_reply_type, redis_reply->str);
}

std::ostream& operator <<(std::ostream& os, const struct NodeInfo& node_info)
{
    os << node_info.id << " " << node_info.ip << ":" << node_info.port << " " << node_info.flags << " "
       << node_info.master_id << " " << node_info.ping_sent << " " << node_info.pong_recv << " " << node_info.epoch << " ";

    if (node_info.connected)
        os << "connected" << " ";
    else
        os << "disconnected" << " ";

    for (std::vector<std::pair<int, int> >::size_type i=0; i<node_info.slots.size(); ++i)
    {
        if (node_info.slots[i].first == node_info.slots[i].second)
            os << node_info.slots[i].first;
        else
            os << node_info.slots[i].first << "-" << node_info.slots[i].second;
    }

    return os;
}

////////////////////////////////////////////////////////////////////////////////
CRedisException::CRedisException(int errcode, const std::string& errmsg, const char* file, int line, const std::string& node_ip, uint16_t node_port, const char* command, const char* key) throw ()
    : _errcode(errcode), _errmsg(errmsg), _file(file), _line(line), _node_ip(node_ip), _node_port(node_port)
{
    if (command != NULL)
        _command = command;
    if (key != NULL)
        _key = key;
}

const char* CRedisException::what() const throw()
{
    return _errmsg.c_str();
}

std::string CRedisException::str() const throw ()
{
    return format_string("redis://%s:%d/%s/%s/%s@%s:%d", _node_ip.c_str(), _node_port, _command.c_str(), _key.c_str(), _errmsg.c_str(), _file.c_str(), _line);
}

////////////////////////////////////////////////////////////////////////////////
struct SlotInfo
{
    int slot;
    redisContext* redis_context;
    std::pair<uint32_t, uint16_t> node;
};

CRedisClient::CRedisClient(const std::string& nodes, int timeout_milliseconds) throw (CRedisException)
    : _cluster_mode(false), _nodes_string(nodes),
      _timeout_milliseconds(timeout_milliseconds), _retry_times(RETRY_TIMES), _retry_sleep_milliseconds(RETRY_SLEEP_MILLISECONDS),
      _redis_context(NULL), _slots(CLUSTER_SLOTS, NULL)
{
    parse_nodes();

    if (_nodes.empty())
    {
        THROW_REDIS_EXCEPTION(ERR_PARAMETER, "invalid nodes or not set");
    }
    else if (_nodes.size() > 1)
    {
        _cluster_mode = true;
        init();
    }
}

CRedisClient::~CRedisClient()
{
    clear();
}

bool CRedisClient::cluster_mode() const
{
    return _cluster_mode;
}

void CRedisClient::set_retry(int retry_times, int retry_sleep_milliseconds)
{
    _retry_times = retry_times;
    _retry_sleep_milliseconds = retry_sleep_milliseconds;

    if (_retry_times < 0)
        _retry_times = 10;
    if (_retry_sleep_milliseconds < 1)
        _retry_sleep_milliseconds = 10;
}

////////////////////////////////////////////////////////////////////////////////
int CRedisClient::list_nodes(std::vector<struct NodeInfo>* nodes_info, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    if (!_cluster_mode)
    {
        return 0;
    }
    else
    {
        int errcode = 0;
        std::string errmsg;
        std::pair<std::string, uint16_t> node;

        for (int i=0; i<static_cast<int>(_nodes.size()); ++i)
        {
            redisReply* redis_reply = NULL;
            redisContext* redis_context = connect_node(&errcode, &errmsg, &node);
            if (NULL == redis_context)
            {
                THROW_REDIS_EXCEPTION_WITH_NODE(errcode, errmsg.c_str(), node.first, node.second);
            }
            else if (which != NULL)
            {
                which->first = node.first;
                which->second = node.second;
            }

            redis_reply = (redisReply*)redisCommand(redis_context, "CLUSTER NODES");
            if (NULL == redis_reply)
            {
                errcode = ERROR_COMMAND;
                errmsg = "redis `CLUSTER NODES` error";
                redisFree(redis_context);
                (*g_debug_log)("[%d](%d)%s", i, errcode, errmsg.c_str());
                continue;
            }
            else if (redis_reply->type != REDIS_REPLY_STRING)
            {
                errcode = redis_reply->type;
                errmsg = redis_reply->str;
                freeReplyObject(redis_reply);
                redisFree(redis_context);
                redis_reply = NULL;
                redis_context = NULL;
                THROW_REDIS_EXCEPTION_WITH_NODE(errcode, errmsg.c_str(), node.first, node.second);
            }

            /*
             * <id> <ip:port> <flags> <master> <ping-sent> <pong-recv> <config-epoch> <link-state> <slot> <slot> ... <slot>
             * flags: A list of comma separated flags: myself, master, slave, fail?, fail, handshake, noaddr, noflags
             * ping-sent: Milliseconds unix time at which the currently active ping was sent, or zero if there are no pending pings
             * pong-recv: Milliseconds unix time the last pong was received
             * link-state: The state of the link used for the node-to-node cluster bus. We use this link to communicate with the node. Can be connected or disconnected
             *
             * `redis_reply->str` example:
             * 56686c7baad565d4370b8f1f6518a67b6cedb210 10.225.168.52:6381 slave 150f77d1000003811fb3c38c3768526a0b25ec31 0 1464662426768 22 connected
             * 150f77d1000003811fb3c38c3768526a0b25ec31 10.225.168.51:6379 myself,master - 0 0 22 connected 3278-5687 11092-11958
             * 6a7709bc680f7b224d0d20bdf7dd14db1f013baf 10.212.2.71:6381 master - 0 1464662429775 24 connected 8795-10922 11959-13107
             */
            std::vector<std::string> lines;
            int num_lines = split(&lines, redis_reply->str, "\n");
            freeReplyObject(redis_reply);
            redisFree(redis_context);
            redis_reply = NULL;
            redis_context = NULL;

            for (int row=0; row<num_lines; ++row)
            {
                std::vector<std::string> tokens;
                const std::string& line = lines[row];
                int num_tokens = split(&tokens, line, " ");
                if (num_tokens >= 8)
                {
                    NodeInfo node_info;
                    node_info.id = tokens[0];
                    if (!parse_node_string(tokens[1], &node_info.ip, &node_info.port))
                    {
                        (*g_debug_log)("invalid node_string: %s\n", tokens[1].c_str());
                    }
                    else
                    {
                        std::string::size_type pos = tokens[2].find("master");

                        node_info.flags = tokens[2];
                        node_info.is_master = (pos != std::string::npos);
                        node_info.master_id = tokens[3];
                        node_info.ping_sent = atoi(tokens[4].c_str());
                        node_info.pong_recv = atoi(tokens[5].c_str());
                        node_info.epoch = atoi(tokens[6].c_str());
                        node_info.connected = (tokens[7] == "connected");

                        for (int col=8; col<num_tokens; ++col)
                        {
                            std::pair<int, int> slot;
                            parse_slot_string(tokens[col], &slot.first, &slot.second);
                            node_info.slots.push_back(slot);
                        }

                        nodes_info->push_back(node_info);
                    } // if (parse_node_string
                } // if (num_tokens >= 8)
            } // for (int row=0; row<num_lines; ++row)
            break;
        }

        if (errcode != 0)
            THROW_REDIS_EXCEPTION_WITH_NODE(errcode, errmsg.c_str(), node.first, node.second);
        return static_cast<int>(nodes_info->size());
    }
}

////////////////////////////////////////////////////////////////////////////////
// KEY VALUE
#if 0
// binary unsafe, key and value can not contain space & LF etc.
bool CRedisClient::exists(const std::string& key, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    int64_t result = 0;

    const std::string command_string = format_string("EXISTS %s", key.c_str());
    const redisReply* redis_reply = redis_command(REDIS_REPLY_INTEGER, which, key, "EXISTS", command_string);
    if (redis_reply != NULL)
    {
        result = redis_reply->integer;
        freeReplyObject(const_cast<redisReply*>(redis_reply));
        redis_reply = NULL;
    }

    return result > 0;
}
#else
// binary safe, key and value can contain any.
bool CRedisClient::exists(const std::string& key, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
	PREPARE_REDIS_COMMAND("EXISTS", sizeof("EXISTS")-1);
	int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
	return result > 0;
}
#endif

bool CRedisClient::expire(const std::string& key, uint32_t seconds, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str6  = any2string(seconds);

    PREPARE_REDIS_COMMAND("EXPIRE", sizeof("EXPIRE")-1);
    str6_ = &str6;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return result > 0;
}

void CRedisClient::set(const std::string& key, const std::string& value, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("SET", sizeof("SET")-1);
    str1_ = &value;
    (void)REDIS_COMMAND(REDIS_REPLY_STATUS);
}

bool CRedisClient::setnx(const std::string& key, const std::string& value, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("SETNX", sizeof("SETNX")-1);
    str1_ = &value;
    return REDIS_COMMAND(REDIS_REPLY_INTEGER) > 0;
}

void CRedisClient::setex(const std::string& key, const std::string& value, uint32_t seconds, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str6 = any2string(seconds);

    PREPARE_REDIS_COMMAND("SETEX", sizeof("SETEX")-1);
    str1_ = &value;
    str6_ = &str6;
    (void)REDIS_COMMAND(REDIS_REPLY_STATUS);
}

bool CRedisClient::get(const std::string& key, std::string* value, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("GET", sizeof("GET")-1);
    value_ = value;
    (void)REDIS_COMMAND(REDIS_REPLY_STRING);
    return !value->empty();
}

bool CRedisClient::del(const std::string& key, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("DEL", sizeof("DEL")-1);
    return REDIS_COMMAND(REDIS_REPLY_INTEGER) > 0;
}

int64_t CRedisClient::incrby(const std::string& key, int64_t increment, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str6 = any2string(increment);

    PREPARE_REDIS_COMMAND("INCRBY", sizeof("INCRBY")-1);
    str6_ = &str6;
    return REDIS_COMMAND(REDIS_REPLY_INTEGER);
}

////////////////////////////////////////////////////////////////////////////////
// LIST
int CRedisClient::llen(const std::string& key, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("LLEN", sizeof("LLEN")-1);
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return static_cast<int>(result);
}

bool CRedisClient::lpop(const std::string& key, std::string* value, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("LPOP", sizeof("LPOP")-1);
    value_ = value;
    (void)REDIS_COMMAND(REDIS_REPLY_STRING);
    return !value->empty();
}

int CRedisClient::lpush(const std::string& key, const std::string& value, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    std::vector<std::string> values(1, value);
    return lpush(key, values, which);
}

int CRedisClient::lpush(const std::string& key, const std::vector<std::string>& values, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("LPUSH", sizeof("LPUSH")-1);
    array_ = &values;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return static_cast<int>(result);
}

int CRedisClient::lrange(const std::string& key, int start, int end, std::vector<std::string>* values, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str6 = any2string(start);
    const std::string str7 = any2string(end);

    PREPARE_REDIS_COMMAND("LRANGE", sizeof("LRANGE")-1);
    str6_ = &str6;
    str7_ = &str7;
    values_ = values;
    (void)REDIS_COMMAND(REDIS_REPLY_ARRAY);
    return static_cast<int>(values->size());
}

bool CRedisClient::ltrim(const std::string& key, int start, int end, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str6 = any2string(start);
    const std::string str7 = any2string(end);

	PREPARE_REDIS_COMMAND("LTRIM", sizeof("LTRIM")-1);
	str6_ = &str6;
    str7_ = &str7;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_STATUS);
    return 1 == result;
}

bool CRedisClient::rpop(const std::string& key, std::string* value, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("RPOP", sizeof("RPOP")-1);
    value_ = value;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_STRING);
    return 1 == result;
}

int CRedisClient::rpush(const std::string& key, const std::string& value, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    std::vector<std::string> values(1, value);
    return rpush(key, value, which);
}

int CRedisClient::rpush(const std::string& key, const std::vector<std::string>& values, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("RPUSH", sizeof("RPUSH")-1);
    array_ = &values;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return result;
}

int CRedisClient::rpushx(const std::string& key, const std::string& value, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("RPUSHX", sizeof("RPUSHX")-1);
    str1_ = &value;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
	return result;
}

////////////////////////////////////////////////////////////////////////////////
// HASH
bool CRedisClient::hdel(const std::string& key, const std::string& field, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    std::vector<std::string> fields(1);
    fields[0] = field;
    return hdel(key, fields, which) > 0;
}

int CRedisClient::hdel(const std::string& key, const std::vector<std::string>& fields, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("HDEL", sizeof("HDEL")-1);
    array_ = &fields;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return static_cast<int>(result);
}

bool CRedisClient::hexists(const std::string& key, const std::string& field, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("HEXISTS", sizeof("HEXISTS")-1);
    str1_ = &field;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return result > 0;
}

int CRedisClient::hlen(const std::string& key, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("HLEN", sizeof("HLEN")-1);
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return static_cast<int>(result);
}

bool CRedisClient::hset(const std::string& key, const std::string& field, const std::string& value, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("HSET", sizeof("HSET")-1);
    str1_ = &field;
    str2_ = &value;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return result > 0;
}

bool CRedisClient::hsetnx(const std::string& key, const std::string& field, const std::string& value, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("HSETNX", sizeof("HSETNX")-1);
    str1_ = &field;
    str2_ = &value;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return result > 0;
}

bool CRedisClient::hget(const std::string& key, const std::string& field, std::string* value, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("HGET", sizeof("HGET")-1);
    str1_ = &field;
    value_ = value;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_STRING);
    return 1 == result;
}

int64_t CRedisClient::hincrby(const std::string& key, const std::string& field, int64_t increment, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str6 = any2string(increment);

    PREPARE_REDIS_COMMAND("HINCRBY", sizeof("HINCRBY")-1);
    str1_ = &field;
    str6_ = &str6;
    return REDIS_COMMAND(REDIS_REPLY_INTEGER);
}

void CRedisClient::hmset(const std::string& key, const std::map<std::string, std::string>& map, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("HMSET", sizeof("HMSET")-1);
    in_map1_ = &map;
    (void)REDIS_COMMAND(REDIS_REPLY_STATUS);
}

int CRedisClient::hmget(const std::string& key, const std::vector<std::string>& fields, std::map<std::string, std::string>* map, bool keep_null, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("HMGET", sizeof("HMGET")-1);
    array_ = &fields;
    out_map_ = map;
    keep_null_ = &keep_null;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_ARRAY);
    return static_cast<int>(result);
}

int CRedisClient::hgetall(const std::string& key, std::map<std::string, std::string>* map, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("HGETALL", sizeof("HGETALL")-1);
    out_map_ = map;
    (void)REDIS_COMMAND(REDIS_REPLY_ARRAY);
    return static_cast<int>(map->size());
}

int CRedisClient::hstrlen(const std::string& key, const std::string& field, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("HSTRLEN", sizeof("HSTRLEN")-1);
    str1_ = &field;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return static_cast<int>(result);
}

int CRedisClient::hkeys(const std::string& key, std::vector<std::string>* fields, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("HKEYS", sizeof("HKEYS")-1);
    values_ = fields;
    (void)REDIS_COMMAND(REDIS_REPLY_ARRAY);
    return static_cast<int>(fields->size());
}

int CRedisClient::hvals(const std::string& key, std::vector<std::string>* vals, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("HVALS", sizeof("HVALS")-1);
    values_ = vals;
    (void)REDIS_COMMAND(REDIS_REPLY_ARRAY);
    return static_cast<int>(vals->size());
}

int CRedisClient::hscan(const std::string& key, int cursor, std::map<std::string, std::string>* map, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str1 = any2string(cursor);

    PREPARE_REDIS_COMMAND("HSCAN", sizeof("HSCAN")-1);
    str1_ = &str1;
    out_map_ = map;
    return REDIS_COMMAND(REDIS_REPLY_ARRAY);
}

int CRedisClient::hscan(const std::string& key, int cursor, int count, std::map<std::string, std::string>* map, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str1 = any2string(cursor);
    const std::string str3 = format_string("COUNT %d", count);

    PREPARE_REDIS_COMMAND("HSCAN", sizeof("HSCAN")-1);
    str1_ = &str1;
    str3_ = &str3;
    out_map_ = map;
    return REDIS_COMMAND(REDIS_REPLY_ARRAY);
}

int CRedisClient::hscan(const std::string& key, int cursor, const std::string& pattern, std::map<std::string, std::string>* map, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str1 = any2string(cursor);
    const std::string str2 = std::string("MATCH ") + pattern;

    PREPARE_REDIS_COMMAND("HSCAN", sizeof("HSCAN")-1);
    str1_ = &str1;
    str2_ = &str2;
    out_map_ = map;
    return REDIS_COMMAND(REDIS_REPLY_ARRAY);
}

int CRedisClient::hscan(const std::string& key, int cursor, const std::string& pattern, int count, std::map<std::string, std::string>* map, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str1 = any2string(cursor);
    const std::string str2 = std::string("MATCH ") + pattern;
    const std::string str3 = format_string("COUNT %d", count);

    PREPARE_REDIS_COMMAND("HSCAN", sizeof("HSCAN")-1);
    str1_ = &str1;
    str2_ = &str2;
    str3_ = &str3;
    out_map_ = map;
    return REDIS_COMMAND(REDIS_REPLY_ARRAY);
}

////////////////////////////////////////////////////////////////////////////////
// SET
int CRedisClient::sadd(const std::string& key, const std::string& value, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("SADD", sizeof("SADD")-1);
    str1_ = &value;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return static_cast<int>(result);
}

int CRedisClient::sadd(const std::string& key, const std::vector<std::string>& values, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("SADD", sizeof("SADD")-1);
    array_ = &values;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return static_cast<int>(result);
}

int CRedisClient::scard(const std::string& key, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("SCARD", sizeof("SCARD")-1);
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return static_cast<int>(result);
}

bool CRedisClient::sismember(const std::string& key, const std::string& value, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("SISMEMBER", sizeof("SISMEMBER")-1);
    str1_ = &value;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return 1 == result;
}

int CRedisClient::smembers(const std::string& key, std::vector<std::string>* values, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("SMEMBERS", sizeof("SMEMBERS")-1);
    values_ = values;
    (void)REDIS_COMMAND(REDIS_REPLY_ARRAY);
    return static_cast<int>(values->size());
}

bool CRedisClient::spop(const std::string& key, std::string* value, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("SPOP", sizeof("SPOP")-1);
    value_ = value;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_STRING);
    return 1 == result;
}

int CRedisClient::spop(const std::string& key, int count, std::vector<std::string>* values, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str1 = any2string(count);

    PREPARE_REDIS_COMMAND("SPOP", sizeof("SPOP")-1);
    str1_ = &str1;
    values_ = values;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_ARRAY);
    return static_cast<int>(result);
}

int CRedisClient::srandmember(const std::string& key, int count, std::vector<std::string>* values, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    std::string str1 = any2string(count);

    PREPARE_REDIS_COMMAND("SRANDMEMBER", sizeof("SRANDMEMBER")-1);
    str1_ = &str1;
    values_ = values;
    (void)REDIS_COMMAND(REDIS_REPLY_ARRAY);
    return static_cast<int>(values->size());
}

int CRedisClient::srem(const std::string& key, const std::string& value, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    std::vector<std::string> values(1);
    values[0] = value;
    return srem(key, values, which);
}

int CRedisClient::srem(const std::string& key, const std::vector<std::string>& values, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("SREM", sizeof("SREM")-1);
    array_ = &values;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return static_cast<int>(result);
}

int CRedisClient::sscan(const std::string& key, int cursor, std::vector<std::string>* values, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str1 = any2string(cursor);

    PREPARE_REDIS_COMMAND("SSCAN", sizeof("SSCAN")-1);
    str1_ = &str1;
    values_ = values;
    (void)REDIS_COMMAND(REDIS_REPLY_ARRAY);
    return static_cast<int>(values->size());
}

int CRedisClient::sscan(const std::string& key, int cursor, int count, std::vector<std::string>* values, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str1 = any2string(cursor);
    const std::string str2 = format_string("COUNT %d", count);

    PREPARE_REDIS_COMMAND("SSCAN", sizeof("SSCAN")-1);
    str1_ = &str1;
    str2_ = &str2;
    values_ = values;
    (void)REDIS_COMMAND(REDIS_REPLY_ARRAY);
    return static_cast<int>(values->size());
}

int CRedisClient::sscan(const std::string& key, int cursor, const std::string& pattern, std::vector<std::string>* values, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str1 = any2string(cursor);
    const std::string str2 = std::string("MATCH ") + pattern;

    PREPARE_REDIS_COMMAND("SSCAN", sizeof("SSCAN")-1);
    str1_ = &str1;
    str2_ = &str2;
    values_ = values;
    (void)REDIS_COMMAND(REDIS_REPLY_ARRAY);
    return static_cast<int>(values->size());
}

int CRedisClient::sscan(const std::string& key, int cursor, const std::string& pattern, int count, std::vector<std::string>* values, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str1 = any2string(cursor);
    const std::string str2 = std::string("MATCH ") + pattern;
    const std::string str3 = format_string("COUNT %d", count);

    PREPARE_REDIS_COMMAND("SSCAN", sizeof("SSCAN")-1);
    str1_ = &str1;
    str2_ = &str2;
    str3_ = &str3;
    values_ = values;
    (void)REDIS_COMMAND(REDIS_REPLY_ARRAY);
    return static_cast<int>(values->size());
}

////////////////////////////////////////////////////////////////////////////////
// SORTED SET
int CRedisClient::zadd(const std::string& key, const std::string& field, int64_t score, ZADDFLAG flag, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    std::map<std::string, int64_t> map;
    map[field] = score;
    return zadd(key, map, flag, which);
}

int CRedisClient::zadd(const std::string& key, const std::map<std::string, int64_t>& map, ZADDFLAG flag, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    std::string str1;
    if (Z_XX == flag)
        str1 = "XX";
    else if (Z_NX == flag)
        str1 = "NX";
    else if (Z_CH == flag)
        str1 = "CH";
    else if (flag != Z_NS)
        THROW_REDIS_EXCEPTION(ERR_PARAMETER, "invalid zadd flag");

    PREPARE_REDIS_COMMAND("ZADD", sizeof("ZADD")-1);
    if (!str1.empty())
        str1_ = & str1;
    in_map2_ = &map;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return static_cast<int>(result);
}

int CRedisClient::zcount(const std::string& key, int64_t min, int64_t max , std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str6 = any2string(min);
    const std::string str7 = any2string(max);

    PREPARE_REDIS_COMMAND("ZCOUNT", sizeof("ZCOUNT")-1);
    str6_ = &str6;
    str7_ = &str7;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return static_cast<int>(result);
}

int64_t CRedisClient::zincrby(const std::string& key, const std::string& field, int64_t increment, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    std::string value;
    std::string str1 = any2string(increment);

    PREPARE_REDIS_COMMAND("ZINCRBY", sizeof("ZINCRBY")-1);
    str1_ = &str1;
    str2_ = &field;
    value_ = &value;
    (void)REDIS_COMMAND(REDIS_REPLY_STRING);
    return atoll(value.c_str());
}

int CRedisClient::zrange(const std::string& key, int start, int end, bool withscores, std::vector<std::pair<std::string, int64_t> >* vec, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str6 = any2string(start);
    const std::string str7 = any2string(end);

    PREPARE_REDIS_COMMAND("ZRANGE", sizeof("ZRANGE")-1);
    str6_ = &str6;
    str7_ = &str7;
    tag_ = "withscores";
    tag_length_ = sizeof("withscores") - 1 ;
    withscores_ = &withscores;
    out_vec_ = vec;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_ARRAY);
    return static_cast<int>(result);
}

int CRedisClient::zrevrange(const std::string& key, int start, int end, bool withscores, std::vector<std::pair<std::string, int64_t> >* vec, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str6 = any2string(start);
    const std::string str7 = any2string(end);

    PREPARE_REDIS_COMMAND("ZREVRANGE", sizeof("ZREVRANGE")-1);
    str6_ = &str6;
    str7_ = &str7;
    tag_ = "withscores";
    tag_length_ = sizeof("withscores") - 1 ;
    withscores_ = &withscores;
    out_vec_ = vec;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_ARRAY);
    return static_cast<int>(result);
}

int CRedisClient::zrank(const std::string& key, const std::string& field, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("ZRANK", sizeof("ZRANK")-1);
    str1_ = &field;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return static_cast<int>(result);
}

int CRedisClient::zrevrank(const std::string& key, const std::string& field, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    PREPARE_REDIS_COMMAND("ZREVRANK", sizeof("ZREVRANK")-1);
    str1_ = &field;
    int64_t result = REDIS_COMMAND(REDIS_REPLY_INTEGER);
    return static_cast<int>(result);
}

int64_t CRedisClient::zscore(const std::string& key, const std::string& field, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    std::string value;

    PREPARE_REDIS_COMMAND("ZSCORE", sizeof("ZSCORE")-1);
    str1_ = &field;
    value_ = &value;
    (void)REDIS_COMMAND(REDIS_REPLY_STRING);
    return atoll(value.c_str());
}

int CRedisClient::zscan(const std::string& key, int cursor, std::vector<std::pair<std::string, int64_t> >* values, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str1 = any2string(cursor);

    PREPARE_REDIS_COMMAND("ZSCAN", sizeof("ZSCAN")-1);
    str1_ = &str1;
    out_vec_ = values;
    return REDIS_COMMAND(REDIS_REPLY_ARRAY);
}

int CRedisClient::zscan(const std::string& key, int cursor, int count, std::vector<std::pair<std::string, int64_t> >* values, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str1 = any2string(cursor);
    const std::string str3 = format_string("COUNT %d", count);

    PREPARE_REDIS_COMMAND("ZSCAN", sizeof("ZSCAN")-1);
    str1_ = &str1;
    str3_ = &str3;
    out_vec_ = values;
    return REDIS_COMMAND(REDIS_REPLY_ARRAY);
}

int CRedisClient::zscan(const std::string& key, int cursor, const std::string& pattern, std::vector<std::pair<std::string, int64_t> >* values, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str1 = any2string(cursor);
    const std::string str2 = format_string("MATCH %s", pattern.c_str());

    PREPARE_REDIS_COMMAND("ZSCAN", sizeof("ZSCAN")-1);
    str1_ = &str1;
    str2_ = &str2;
    out_vec_ = values;
    return REDIS_COMMAND(REDIS_REPLY_ARRAY);
}

int CRedisClient::zscan(const std::string& key, int cursor, const std::string& pattern, int count, std::vector<std::pair<std::string, int64_t> >* values, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
    const std::string str1 = any2string(cursor);
    const std::string str2 = format_string("MATCH %s", pattern.c_str());
    const std::string str3 = format_string("COUNT %d", count);

    PREPARE_REDIS_COMMAND("ZSCAN", sizeof("ZSCAN")-1);
    str1_ = &str1;
    str2_ = &str2;
    str3_ = &str3;
    out_vec_ = values;
    return REDIS_COMMAND(REDIS_REPLY_ARRAY);
}


////////////////////////////////////////////////////////////////////////////////
// RAW COMMAND
const redisReply* CRedisClient::redis_command(int excepted_reply_type, std::pair<std::string, uint16_t>* which, const std::string& key, const char* command, const std::string& command_string) throw (CRedisException)
{
    return redis_command(excepted_reply_type, which, key, command, command_string, 0, NULL, NULL);
}

const redisReply* CRedisClient::redis_command(int excepted_reply_type, std::pair<std::string, uint16_t>* which, const std::string& key, const char* command, int argc, const char* argv[], const size_t* argv_len) throw (CRedisException)
{
    const std::string command_string;
    return redis_command(excepted_reply_type, which, key, command, command_string, argc, argv, argv_len);
}

const redisReply* CRedisClient::redis_command(int excepted_reply_type, std::pair<std::string, uint16_t>* which, const std::string& key, const char* command, const std::string& command_string, int argc, const char* argv[], const size_t* argv_len) throw (CRedisException)
{
    redisReply* redis_reply = NULL;
    int errcode = 0;
    std::string errmsg;
    std::pair<std::string, uint16_t> node;
    unsigned int slot = _cluster_mode? keyHashSlot(key.c_str(), key.size()): 0;

    for (int i=0; i<_retry_times; ++i)
    {
        errcode = 0;
        redisContext* redis_context = get_redis_context(slot, &node);
        if (NULL == redis_context)
        {
            THROW_REDIS_EXCEPTION_WITH_NODE_AND_COMMAND(ERR_INIT_REDIS_CONN, "connect redis cluster failed", node.first, node.second, command, key.c_str());
        }

        if (which != NULL)
        {
            which->first = node.first;
            which->second = node.second;
        }
        if (redis_context->err != 0)
        {
            errcode = redis_context->err;
            errmsg = redis_context->errstr;
            (*g_debug_log)("[%d][%s](%d)%s\n", i, command, errcode, errmsg.c_str());

            init();
            retry_sleep();
            continue;
        }

        if (0 == argc)
            redis_reply = (redisReply*)redisCommand(redis_context, command_string.c_str());
        else
            redis_reply = (redisReply*)redisCommandArgv(redis_context, argc, argv, argv_len);
        if (NULL == redis_reply)
        {
            // disconnnected
            errcode = ERROR_COMMAND;
            errmsg = format_string("redis `%s` error", command);
            init();
            retry_sleep();
            continue;
        }
        else
        {
            print_reply(command, key.c_str(), slot, redis_reply, excepted_reply_type, node);

            if (REDIS_REPLY_NIL == redis_reply->type)
            {
                break;
            }
            else if (redis_reply->type != excepted_reply_type)
            {
                //#define REDIS_REPLY_ERROR 6
                int errcode = redis_reply->type;
                std::string errmsg;
                if (redis_reply->str != NULL)
                    errmsg = redis_reply->str;
                freeReplyObject(redis_reply);
                redis_reply = NULL;
                THROW_REDIS_EXCEPTION_WITH_NODE_AND_COMMAND(errcode, errmsg.c_str(), redis_context->tcp.host, redis_context->tcp.port, command, key.c_str());
            }

            break;
        }
    }

    if (errcode != 0)
    {
        if (redis_reply != NULL)
        {
            freeReplyObject(redis_reply);
            redis_reply = NULL;
        }
        THROW_REDIS_EXCEPTION_WITH_NODE_AND_COMMAND(errcode, errmsg, node.first, node.second, command, key.c_str());
    }
    return redis_reply;
}

int64_t CRedisClient::redis_command(int excepted_reply_type, const char* command, size_t command_length, const std::string* key, const std::string* str1, const std::string* str2, const std::string* str3, const std::vector<std::string>* array, const std::map<std::string, std::string>* in_map1, const std::map<std::string, int64_t>* in_map2, const std::string* str6, const std::string* str7, const char* tag, size_t tag_length, std::string* value, std::vector<std::string>* values, std::map<std::string, std::string>* out_map, std::vector<std::pair<std::string, int64_t> >* out_vec, const bool* keep_null, const bool* withscores, std::pair<std::string, uint16_t>* which) throw (CRedisException)
{
	int64_t result = 0;
	size_t elements;
	size_t i;
    struct redisReply** element;

	int index = 0;
    int argc = calc_argc(key, str1, str2, str3, array, in_map1, in_map2, str6, str7, tag);
    char** argv = new char*[argc];
	size_t* argv_len = new size_t[argc];
	std::string str;

    // command
    argv_len[index] = command_length;
    argv[index] = new char[argv_len[index]];
    memcpy(argv[index], command, argv_len[index]);
    ++index;

    // key
    if (key != NULL)
    {
        argv_len[index] = (*key).size();
        argv[index] = new char[argv_len[index]];
        memcpy(argv[index], (*key).c_str(), argv_len[index]);
        ++index;
    }

    // str1
    if (str1 != NULL)
    {
        argv_len[index] = (*str1).size();
        argv[index] = new char[argv_len[index]];
        memcpy(argv[index], (*str1).c_str(), argv_len[index]);
        ++index;
    }

    // str2
    if (str2 != NULL)
    {
        argv_len[index] = (*str2).size();
        argv[index] = new char[argv_len[index]];
        memcpy(argv[index], (*str2).c_str(), argv_len[index]);
        ++index;
    }

    // str3
    if (str3 != NULL)
    {
        argv_len[index] = (*str3).size();
        argv[index] = new char[argv_len[index]];
        memcpy(argv[index], (*str3).c_str(), argv_len[index]);
        ++index;
    }

    // array
    if (array != NULL)
    {
		for (i=0; i<array->size(); ++i)
		{
			argv_len[index] = (*array)[i].size();
			argv[index] = new char[argv_len[index]];
			memcpy(argv[index], (*array)[i].c_str(), argv_len[index]);
			++index;
		}
    }

    // in_map1
    if (in_map1 != NULL)
    {
        for (std::map<std::string, std::string>::const_iterator c_iter=(*in_map1).begin(); c_iter!=(*in_map1).end(); ++c_iter)
        {
            argv_len[index] = c_iter->first.size();
            argv[index] = new char[argv_len[index]];
            memcpy(argv[index], c_iter->first.c_str(), argv_len[index]);
            ++index;

            argv_len[index] = c_iter->second.size();
            argv[index] = new char[argv_len[index]];
            memcpy(argv[index], c_iter->second.c_str(), argv_len[index]);
            ++index;
        }
    }

    // in_map2
    if (in_map2 != NULL)
    {
        for (std::map<std::string, int64_t>::const_iterator c_iter=(*in_map2).begin(); c_iter!=(*in_map2).end(); ++c_iter)
        {
            // score
            str = any2string(c_iter->second);
            argv_len[index] = str.size();
            argv[index] = new char[argv_len[index]];
            memcpy(argv[index], str.c_str(), argv_len[index]);
            ++index;

            // value
            argv_len[index] = c_iter->first.size();
            argv[index] = new char[argv_len[index]];
            memcpy(argv[index], c_iter->first.c_str(), argv_len[index]);
            ++index;
        }
    }

    // str6
    if (str6 != NULL)
    {
        argv_len[index] = str6->size();
        argv[index] = new char[argv_len[index]];
        memcpy(argv[index], str6->c_str(), argv_len[index]);
        ++index;
    }

    // str7
    if (str7)
    {
        argv_len[index] = str7->size();
        argv[index] = new char[argv_len[index]];
        memcpy(argv[index], str7->c_str(), argv_len[index]);
        ++index;
    }

    // tag
    if (tag != NULL)
    {
        argv_len[index] = tag_length;
        argv[index] = new char[argv_len[index]];
        memcpy(argv[index], tag, argv_len[index]);
        ++index;
    }

    // value
    if (value != NULL) value->clear();
    // values
    if (values != NULL) values->clear();
    // out_map
    if (out_map != NULL) out_map->clear();
    // out_vec
    if (out_vec != NULL) out_vec->clear();

    const FreeArgvHelper fah(argc, argv, argv_len);
    const redisReply* redis_reply = redis_command(excepted_reply_type, which, *key, command, argc, (const char**)argv, argv_len);
    if (redis_reply != NULL)
    {
        if (REDIS_REPLY_STATUS == redis_reply->type)
        {
            if (0 == strcmp(redis_reply->str, "OK"))
                result = 1;
        }
        else if (REDIS_REPLY_INTEGER == redis_reply->type)
    	{
    		result = redis_reply->integer;
    	}
    	else if (REDIS_REPLY_STRING == redis_reply->type)
    	{
    	    R3C_ASSERT(value != NULL);

    		result = 1;
    		value->assign(redis_reply->str, redis_reply->len);
    	}
    	else if (REDIS_REPLY_ARRAY == redis_reply->type)
    	{
    		if (values != NULL)
    		{
    		    R3C_ASSERT(NULL == out_map);

    		    if (strcasecmp(command, "sscan") != 0)
    		    {
    		        elements = redis_reply->elements;
    		        element = redis_reply->element;
    		        result = static_cast<int64_t>(elements);
    		    }
    		    else
    		    {
    		        R3C_ASSERT(2 == redis_reply->elements);

    		        elements = redis_reply->element[1]->elements;
    		        element = redis_reply->element[1]->element;
    		        result = redis_reply->element[0]->integer; // cursor
    		    }

    			values->resize(elements);
    	        for (i=0; i<elements; ++i)
    	        {
    	            const std::string v(element[i]->str, element[i]->len);
    	            (*values)[i] = v;
    	        }
    		}
    		else if (out_vec != NULL) // zrange
            {
    		    if (strcasecmp(command, "zscan") != 0)
    		    {
                    for (i=0; i<redis_reply->elements;)
                    {
                        ++result;

                        const std::string k(redis_reply->element[i]->str, redis_reply->element[i]->len);
                        if (!*withscores)
                        {
                            out_vec->push_back(std::make_pair(k, 0));
                            i += 1;
                        }
                        else
                        {
                            const std::string v(redis_reply->element[i+1]->str, redis_reply->element[i+1]->len);
                            out_vec->push_back(std::make_pair(k, atoll(v.c_str())));
                            i += 2;
                        }
                    }
    		    }
    		    else
    		    {
    		        result = redis_reply->element[0]->integer; // cursor

    		        for (i=0; i<redis_reply->element[1]->elements; i+=2)
    		        {
    		            const std::string k(redis_reply->element[1]->element[i]->str, redis_reply->element[1]->element[i]->len);
    		            const std::string v(redis_reply->element[1]->element[i+1]->str, redis_reply->element[1]->element[i+1]->len);
    		            out_vec->push_back(std::make_pair(k, atoll(v.c_str())));
    		        }
    		    }
            } // zrange
    		else if (out_map != NULL) // hmget & hgetall
    		{
    		    if (NULL == keep_null) // hgetall
    		    {
    		        if (strcasecmp(command, "hscan") != 0)
    		        {
    		            elements = redis_reply->elements;
    		            element = redis_reply->element;
    		            result = static_cast<int64_t>(elements / 2);
    		        }
    		        else
    		        {
    		            elements = redis_reply->element[1]->elements;
    		            element = redis_reply->element[1]->element;
    		            result = redis_reply->element[1]->integer; // cursor
    		        }

                    for (i=0; i<elements; i+=2)
                    {
                        const std::string k(element[i]->str, element[i]->len);
                        const std::string v(element[i+1]->str, element[i+1]->len);
                        (*out_map)[k] = v;
                    }
    		    } // hgetall
    		    else // hmget
    			{
    		        R3C_ASSERT(array != NULL);

					for (i=0; i<redis_reply->elements; ++i)
					{
						if (REDIS_REPLY_STRING == redis_reply->element[i]->type)
						{
							++result;
							const std::string value(redis_reply->element[i]->str, redis_reply->element[i]->len);
							(*out_map)[(*array)[i]] = value;
						}
						else if (REDIS_REPLY_INTEGER == redis_reply->element[i]->type)
						{
							++result;
							(*out_map)[(*array)[i]] = any2string(redis_reply->element[i]->integer);
						}
						else if (REDIS_REPLY_NIL == redis_reply->element[i]->type)
                        {
                            if (*keep_null)
                                (*out_map)[(*array)[i]] = std::string("");
                        }
					}
    			} // hmget
    		} // hmget & hgetall
    	}

        freeReplyObject(const_cast<redisReply*>(redis_reply));
        redis_reply = NULL;
    }

	return result;
}

int CRedisClient::calc_argc(const std::string* key, const std::string* str1, const std::string* str2, const std::string* str3, const std::vector<std::string>* array, const std::map<std::string, std::string>* in_map1, const std::map<std::string, int64_t>* in_map2, const std::string* str6, const std::string* str7, const char* tag) const
{
	size_t argc = 1;
	if (key != NULL) argc += 1;

	if (str1 != NULL) argc += 1;
	if (str2 != NULL) argc += 1;
	if (str3 != NULL) argc += 1;

	if (array != NULL) argc += array->size();
	if (in_map1 != NULL) argc += in_map1->size() + in_map1->size();
	if (in_map2 != NULL) argc += in_map2->size() + in_map2->size();

	if (str6 != NULL) argc += 1;
	if (str7 != NULL) argc += 1;

	if (tag != NULL) argc += 1;
	return static_cast<int>(argc);
}

////////////////////////////////////////////////////////////////////////////////
void CRedisClient::parse_nodes() throw (CRedisException)
{
    std::string::size_type len = 0;
    std::string::size_type pos = 0;
    std::string::size_type comma_pos = 0;

    _nodes.clear();
    while (comma_pos != std::string::npos)
    {
        comma_pos = _nodes_string.find(',', pos);
        if (comma_pos != std::string::npos)
            len = comma_pos - pos;
        else
            len = _nodes_string.size() - comma_pos;

        const std::string str = _nodes_string.substr(pos, len);
        std::string::size_type colon_pos = str.find(':');
        if (colon_pos == std::string::npos)
            THROW_REDIS_EXCEPTION(ERR_PARAMETER, "parameter[nodes] error");

        const std::string ip = str.substr(0, colon_pos);
        const std::string port = str.substr(colon_pos+1);
        _nodes.push_back(std::make_pair(ip, atoi(port.c_str())));

        // Next node
        pos = comma_pos + 1;
    }
}

void CRedisClient::init() throw (CRedisException)
{
    clear();

    if (_cluster_mode)
    {
        std::string nodes_string;
        std::vector<struct NodeInfo> nodes_info;
        int num_nodes = list_nodes(&nodes_info);

        for (int i=0; i<num_nodes; ++i)
        {
            const struct NodeInfo& node_info = nodes_info[i];

            for (std::vector<std::pair<int, int> >::size_type j=0; j<node_info.slots.size(); ++j)
            {
                if (nodes_string.empty())
                    nodes_string = format_string("%s:%d", node_info.ip.c_str(), node_info.port);
                else
                    nodes_string += format_string(",%s:%d", node_info.ip.c_str(), node_info.port);

                for (int slot=node_info.slots[j].first; slot<=node_info.slots[j].second; ++slot)
                {
                    struct SlotInfo* slot_info = new struct SlotInfo;
                    slot_info->slot = slot;
                    slot_info->redis_context = NULL;
                    slot_info->node.first = inet_addr(node_info.ip.c_str());
                    slot_info->node.second = node_info.port;

                    _slots[slot] = slot_info;
                }
            }
        }

        if (!nodes_string.empty())
        {
            _nodes_string = nodes_string;
            parse_nodes();
        }
    }
}

redisContext* CRedisClient::get_redis_context(unsigned int slot, std::pair<std::string, uint16_t>* node) throw (CRedisException)
{
    redisContext* redis_context = NULL;

    if (!_cluster_mode)
    {
        node->first = _nodes[0].first;
        node->second = _nodes[0].second;

        if (_redis_context != NULL)
        {
            redis_context = _redis_context;
        }
        else
        {
            if (_timeout_milliseconds < 0)
            {
                redis_context = redisConnect(node->first.c_str(), node->second);
            }
            else
            {
                struct timeval tv;
                tv.tv_sec = _timeout_milliseconds / 1000;
                tv.tv_usec = (_timeout_milliseconds % 1000) * 1000;
                redis_context = redisConnectWithTimeout(node->first.c_str(), node->second, tv);
            }

            _redis_context = redis_context;
        }
    }
    else
    {
        R3C_ASSERT(slot < CLUSTER_SLOTS);

        if (slot < CLUSTER_SLOTS)
        {
            struct in_addr in;
            struct SlotInfo* slot_info = _slots[slot];
            if (NULL == slot_info)
            {
                const std::string errmsg = format_string("slot[%u] not exists", slot);
                THROW_REDIS_EXCEPTION(ERROR_SLOT_NOT_EXIST, errmsg.c_str());
            }

            in.s_addr = slot_info->node.first;
            node->first = inet_ntoa(in);
            node->second = slot_info->node.second;

            if (slot_info->redis_context != NULL)
            {
                redis_context = slot_info->redis_context;
            }
            else
            {
                std::map<std::pair<uint32_t, uint16_t>, redisContext*>::const_iterator iter = _redis_contexts.find(slot_info->node);
                if (iter != _redis_contexts.end())
                {
                    redis_context = iter->second;
                    slot_info->redis_context = redis_context;
                }
                else
                {
                    if (_timeout_milliseconds < 0)
                    {
                        redis_context = redisConnect(node->first.c_str(), node->second);
                    }
                    else
                    {
                        struct timeval tv;
                        tv.tv_sec = _timeout_milliseconds / 1000;
                        tv.tv_usec = (_timeout_milliseconds % 1000) * 1000;
                        redis_context = redisConnectWithTimeout(node->first.c_str(), node->second, tv);
                    }

                    slot_info->redis_context = redis_context;
                    _redis_contexts.insert(std::make_pair(slot_info->node, redis_context));
                }
            } // if (slot_info->redis_context != NULL)
        } // if (slot < CLUSTER_SLOTS)
    } // cluster_mode

    return redis_context;
}

void CRedisClient::choose_node(int seed_factor, std::pair<std::string, uint16_t>* node) const
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srandom(tv.tv_usec + seed_factor);

    long index = random() % _nodes.size();
    node->first = _nodes[index].first;
    node->second = _nodes[index].second;
}

redisContext* CRedisClient::connect_node(int* errcode, std::string* errmsg, std::pair<std::string, uint16_t>* node) const
{
    for (std::vector<std::pair<std::string, uint16_t> >::size_type i=0; i<_nodes.size(); ++i)
    {
        choose_node(i, node);

        redisContext* redis_context = redisConnect(node->first.c_str(), node->second);
        if (redis_context != NULL)
        {
            if ('\0' == redis_context->errstr[0])
                return redis_context;

            *errcode = redis_context->err;
            *errmsg = redis_context->errstr;
            redisFree(redis_context);
        }
    }

    return NULL;
}

void CRedisClient::clear()
{
    if (_cluster_mode)
    {
        clear_redis_contexts();
        clear_slots();
    }
    else if (_redis_context != NULL)
    {
        redisFree(_redis_context);
        _redis_context = NULL;
    }
}

void CRedisClient::clear_redis_contexts()
{
    for (std::map<std::pair<uint32_t, uint16_t>, redisContext*>::iterator iter=_redis_contexts.begin(); iter!=_redis_contexts.end(); ++iter)
    {
        redisContext* redis_context = iter->second;
        redisFree(redis_context);
    }
    _redis_contexts.clear();
}

void CRedisClient::clear_slots()
{
    for (int i=0; i<CLUSTER_SLOTS; ++i)
    {
        struct SlotInfo* slot_info = _slots[i];
        delete slot_info;
        _slots[i] = NULL;
    }
}

void CRedisClient::retry_sleep() const
{
    if (_retry_sleep_milliseconds > 0)
        millisleep(_retry_sleep_milliseconds);
}

} // namespace r3c {
