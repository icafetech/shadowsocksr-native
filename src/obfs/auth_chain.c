#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>  // for LLONG_MIN and LLONG_MAX
#include <assert.h>
#include "auth.h"
#include "obfsutil.h"
#include "crc32.h"
#include "base64.h"
#include "encrypt.h"
#include "ssrbuffer.h"
#include "obfs.h"
#include "auth_chain.h"

void auth_chain_a_dispose(struct obfs_t *obfs);
void * auth_chain_a_init_data(void);
size_t auth_chain_a_get_overhead(struct obfs_t *obfs);
void auth_chain_a_set_server_info(struct obfs_t *obfs, struct server_info_t *server);

size_t auth_chain_a_client_pre_encrypt(struct obfs_t *obfs, char **pplaindata, size_t datalength, size_t* capacity);
ssize_t auth_chain_a_client_post_decrypt(struct obfs_t *obfs, char **pplaindata, int datalength, size_t* capacity);
ssize_t auth_chain_a_client_udp_pre_encrypt(struct obfs_t *obfs, char **pplaindata, size_t datalength, size_t* capacity);
ssize_t auth_chain_a_client_udp_post_decrypt(struct obfs_t *obfs, char **pplaindata, size_t datalength, size_t* capacity);

struct buffer_t * auth_chain_a_server_pre_encrypt(struct obfs_t *obfs, const struct buffer_t *buf);
struct buffer_t * auth_chain_a_server_post_decrypt(struct obfs_t *obfs, struct buffer_t *buf, bool *need_feedback);

#if defined(_MSC_VER) && (_MSC_VER < 1800)

/*
 * Convert a string to a long long integer.
 *
 * Ignores `locale' stuff.  Assumes that the upper and lower case
 * alphabets and digits are each contiguous.
 *
 * https://github.com/gcc-mirror/gcc/blob/master/libiberty/strtoll.c
 */

#include <ctype.h>
#include <limits.h>
static long long strtoll(const char *nptr, char **endptr, register int base) {
#pragma warning(push)
#pragma warning(disable: 4146)
    register const char *s = nptr;
    register unsigned long long acc;
    register int c;
    register unsigned long long cutoff;
    register int neg = 0, any, cutlim;

    do {
        c = *s++;
    } while (isspace(c));
    if (c == '-') {
        neg = 1;
        c = *s++;
    } else if (c == '+') {
        c = *s++;
    }
    if ((base == 0 || base == 16) && c == '0' && (*s == 'x' || *s == 'X')) {
        c = s[1];
        s += 2;
        base = 16;
    }
    if (base == 0) {
        base = c == '0' ? 8 : 10;
    }
    cutoff = neg ? -(unsigned long long)LLONG_MIN : LLONG_MAX;
    cutlim = cutoff % (unsigned long long)base;
    cutoff /= (unsigned long long)base;
    for (acc = 0, any = 0;; c = *s++) {
        if (isdigit(c)) {
            c -= '0';
        } else if (isalpha(c)) {
            c -= isupper(c) ? 'A' - 10 : 'a' - 10;
        } else {
            break;
        }
        if (c >= base) {
            break;
        }
        if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim)) {
            any = -1;
        } else {
            any = 1;
            acc *= base;
            acc += c;
        }
    }
    if (any < 0) {
        acc = neg ? LLONG_MIN : LLONG_MAX;
        // errno = ERANGE;
    } else if (neg) {
        acc = -acc;
    }
    if (endptr != 0) {
        *endptr = (char *) (any ? s - 1 : nptr);
    }
    return (acc);
#pragma warning(pop)
}

#endif // defined(_MSC_VER) && (_MSC_VER < 1800)


uint32_t g_endian_test = 1;

struct shift128plus_ctx {
    uint64_t v[2];
};

uint64_t shift128plus_next(struct shift128plus_ctx *ctx) {
    uint64_t x = ctx->v[0];
    uint64_t y = ctx->v[1];
    ctx->v[0] = y;
    x ^= x << 23;
    x ^= (y ^ (x >> 17) ^ (y >> 26));
    ctx->v[1] = x;
    return x + y;
}

void i64_memcpy(uint8_t* target, uint8_t* source)
{
    int i = 0;
    for (i = 0; i < 8; ++i) {
        target[i] = source[7 - i];
    }
}

void shift128plus_init_from_bin(struct shift128plus_ctx *ctx, uint8_t *bin, int bin_size) {
    uint8_t fill_bin[16] = {0};
    memcpy(fill_bin, bin, bin_size);
    if (*(uint8_t*)(&g_endian_test) == 1) {
        memcpy(ctx, fill_bin, 16);
    } else {
        i64_memcpy((uint8_t*)ctx, fill_bin);
        i64_memcpy((uint8_t*)ctx + 8, fill_bin + 8);
    }
}

void shift128plus_init_from_bin_datalen(struct shift128plus_ctx *ctx, const uint8_t* bin, int bin_size, int datalen) {
    int i = 0;
    uint8_t fill_bin[16] = {0};
    memcpy(fill_bin, bin, bin_size);
    fill_bin[0] = (uint8_t)datalen;
    fill_bin[1] = (uint8_t)(datalen >> 8);
    if (*(uint8_t*)&g_endian_test == 1) {
        memcpy(ctx, fill_bin, 16);
    } else {
        i64_memcpy((uint8_t*)ctx, fill_bin);
        i64_memcpy((uint8_t*)ctx + 8, fill_bin + 8);
    }
    
    for (i = 0; i < 4; ++i) {
        shift128plus_next(ctx);
    }
}

struct auth_chain_global_data {
    uint8_t local_client_id[4];
    uint32_t connection_id;
};

struct auth_chain_b_context {
    int    *data_size_list;
    size_t  data_size_list_length;
    int    *data_size_list2;
    size_t  data_size_list2_length;
    void *subclass_context;
};

struct auth_chain_c_context {
    int    *data_size_list0;
    size_t  data_size_list0_length;
    void *subclass_context;
};

struct auth_chain_a_context {
    struct obfs_t * obfs;
    int has_sent_header;
    bool has_recv_header;
    struct buffer_t *recv_buffer;
    uint32_t recv_id;
    uint32_t pack_id;
    char * salt;
    struct buffer_t *user_key;
    char uid[4];
    int last_data_len;
    uint8_t last_client_hash[16];
    uint8_t last_server_hash[16];
    struct shift128plus_ctx random_client;
    struct shift128plus_ctx random_server;
    struct cipher_env_t *cipher;
    struct enc_ctx *encrypt_ctx;
    struct enc_ctx *decrypt_ctx;
    uint32_t user_id_num;
    uint16_t client_over_head;
    size_t unit_len;
    int max_time_dif;
    uint32_t client_id;
    uint32_t connection_id;

    // rnd_data_len
    unsigned int (*get_tcp_rand_len)(struct auth_chain_a_context *local, int datalength, struct shift128plus_ctx *random, const uint8_t last_hash[16]);
    void *subclass_context;
};

void auth_chain_a_context_init(struct obfs_t *obfs, struct auth_chain_a_context *local) {
    local->obfs = obfs;
    local->has_sent_header = 0;
    local->recv_buffer = buffer_alloc(16384);
    local->recv_id = 1;
    local->pack_id = 1;
    local->salt = "";
    local->user_key = buffer_alloc(SSR_BUFF_SIZE);
    memset(&local->random_client, 0, sizeof(local->random_client));
    memset(&local->random_server, 0, sizeof(local->random_server));
    local->encrypt_ctx = NULL;
    local->decrypt_ctx = NULL;
    local->get_tcp_rand_len = NULL;
    local->subclass_context = NULL;
    local->max_time_dif = 60 * 60 * 24; // time dif (second) setting
}

unsigned int auth_chain_a_get_rand_len(struct auth_chain_a_context *local, int datalength, struct shift128plus_ctx *random, const uint8_t last_hash[16]);
unsigned int get_rand_start_pos(int rand_len, struct shift128plus_ctx *random);

int data_size_list_compare(const void *a, const void *b) {
    return (*(int *)a - *(int *)b);
}

void * auth_chain_a_init_data(void) {
    struct auth_chain_global_data *global = (struct auth_chain_global_data*)malloc(sizeof(struct auth_chain_global_data));
    rand_bytes(global->local_client_id, 4);
    rand_bytes((uint8_t*)(&global->connection_id), 4);
    global->connection_id &= 0xFFFFFF;
    return global;
}

void auth_chain_a_new_obfs(struct obfs_t *obfs) {
    struct auth_chain_a_context *auth_chain_a = (struct auth_chain_a_context *)
        calloc(1, sizeof(struct auth_chain_a_context));

    auth_chain_a_context_init(obfs, auth_chain_a);
    auth_chain_a->salt = "auth_chain_a";
    auth_chain_a->get_tcp_rand_len = auth_chain_a_get_rand_len;

    obfs->l_data = auth_chain_a;

    obfs->init_data = auth_chain_a_init_data;
    obfs->get_overhead = auth_chain_a_get_overhead;
    obfs->need_feedback = need_feedback_true;
    obfs->get_server_info = get_server_info;
    obfs->set_server_info = auth_chain_a_set_server_info;
    obfs->dispose = auth_chain_a_dispose;

    obfs->client_pre_encrypt = auth_chain_a_client_pre_encrypt;
    obfs->client_post_decrypt = auth_chain_a_client_post_decrypt;
    obfs->client_udp_pre_encrypt = auth_chain_a_client_udp_pre_encrypt;
    obfs->client_udp_post_decrypt = auth_chain_a_client_udp_post_decrypt;

    obfs->server_pre_encrypt = auth_chain_a_server_pre_encrypt;
    obfs->server_post_decrypt = auth_chain_a_server_post_decrypt;
}

size_t auth_chain_a_get_overhead(struct obfs_t *obfs) {
    return 4;
}

void auth_chain_a_dispose(struct obfs_t *obfs) {
    struct auth_chain_a_context *local = (struct auth_chain_a_context*)obfs->l_data;
    buffer_free(local->recv_buffer);
    buffer_free(local->user_key);
    if (local->cipher) {
        enc_ctx_release_instance(local->cipher, local->encrypt_ctx);
        enc_ctx_release_instance(local->cipher, local->decrypt_ctx);
        cipher_env_release(local->cipher);
    }
    free(local);
    obfs->l_data = NULL;
    dispose_obfs(obfs);
}

void auth_chain_a_set_server_info(struct obfs_t * obfs, struct server_info_t * server) {
    //
    // Don't change server.overhead in here. The server.overhead are counted from the ssrcipher.c#L176
    // The input's server.overhead is the total server.overhead that sum of all the plugin's overhead
    //
    // server->overhead = 4;
    set_server_info(obfs, server);
}

unsigned int auth_chain_a_get_rand_len(struct auth_chain_a_context *local, int datalength, struct shift128plus_ctx *random, const uint8_t last_hash[16]) {
    if (datalength > 1440) {
        return 0;
    }
    shift128plus_init_from_bin_datalen(random, last_hash, 16, datalength);
    if (datalength > 1300) {
        return shift128plus_next(random) % 31;
    }
    if (datalength > 900) {
        return shift128plus_next(random) % 127;
    }
    if (datalength > 400) {
        return shift128plus_next(random) % 521;
    }
    return shift128plus_next(random) % 1021;
}

struct buffer_t * auth_chain_a_rnd_data(struct obfs_t * obfs, 
    const struct buffer_t *buf, struct shift128plus_ctx *random, 
    const uint8_t last_hash[16])
{
    struct auth_chain_a_context *local = (struct auth_chain_a_context *) obfs->l_data;
    struct server_info_t *server = &obfs->server;
    size_t rand_len = local->get_tcp_rand_len(local, (int) buf->len, random, last_hash);
    struct buffer_t *rnd_data_buf = buffer_alloc(rand_len);
    struct buffer_t *ret = NULL;

    rand_bytes(rnd_data_buf->buffer, (int)rand_len);
    rnd_data_buf->len = rand_len;

    do {
        if (buf->len == 0) {
            ret = buffer_clone(rnd_data_buf);
            break;
        } 
        if (rand_len > 0) {
            size_t start_pos = (size_t) get_rand_start_pos((int)rand_len, random);
            ret = buffer_create_from(rnd_data_buf->buffer, start_pos);
            buffer_concatenate2(ret, buf);
            buffer_concatenate(ret, rnd_data_buf->buffer + start_pos, rnd_data_buf->len - start_pos);
            break;
        } else {
            ret = buffer_clone(buf);
            break;
        }
    } while(0);
    buffer_free(rnd_data_buf);
    return ret;
}

size_t auth_chain_find_pos(int *arr, size_t length, int key) {
    size_t low = 0;
    size_t high = length - 1;
    size_t middle = -1;

    if (key > arr[high]) {
        return length;
    }
    while (low < high) {
        middle = (low + high) / 2;
        if (key > arr[middle]) {
            low = middle + 1;
        } else if (key <= arr[middle]) {
            high = middle;
        }
    }
    return low;
}

unsigned int udp_get_rand_len(struct shift128plus_ctx *random, uint8_t last_hash[16]) {
    shift128plus_init_from_bin(random, last_hash, 16);
    return shift128plus_next(random) % 127;
}

unsigned int get_rand_start_pos(int rand_len, struct shift128plus_ctx *random) {
    if (rand_len > 0) {
        return (unsigned int)(shift128plus_next(random) % 8589934609 % (uint64_t)rand_len);
    }
    return 0;
}

unsigned int get_client_rand_len(struct auth_chain_a_context *local, size_t datalength) {
    return local->get_tcp_rand_len(local, (int)datalength, &local->random_client, local->last_client_hash);
}

unsigned int get_server_rand_len(struct auth_chain_a_context *local, int datalength) {
    return local->get_tcp_rand_len(local, datalength, &local->random_server, local->last_server_hash);
}

size_t auth_chain_a_pack_client_data(struct obfs_t *obfs, char *data, size_t datalength, char *outdata) {
    uint8_t key_len;
    uint8_t *key;
    struct auth_chain_a_context *local = (struct auth_chain_a_context *) obfs->l_data;
    struct server_info_t *server = &obfs->server;

    unsigned int rand_len = get_client_rand_len(local, datalength);
    size_t out_size = (size_t)rand_len + datalength + 2;
    outdata[0] = (char)((uint8_t)datalength ^ local->last_client_hash[14]);
    outdata[1] = (char)((uint8_t)(datalength >> 8) ^ local->last_client_hash[15]);

    {
        uint8_t * rnd_data = (uint8_t *) malloc(rand_len * sizeof(uint8_t));
        rand_bytes(rnd_data, (int)rand_len);
        if (datalength > 0) {
            unsigned int start_pos = get_rand_start_pos((int)rand_len, &local->random_client);
            size_t out_len;
            ss_encrypt_buffer(local->cipher, local->encrypt_ctx,
                    data, (size_t)datalength, &outdata[2 + start_pos], &out_len);
            memcpy(outdata + 2, rnd_data, start_pos);
            memcpy(outdata + 2 + start_pos + datalength, rnd_data + start_pos, rand_len - start_pos);
        } else {
            memcpy(outdata + 2, rnd_data, rand_len);
        }
        free(rnd_data);
    }

    key_len = (uint8_t)(local->user_key->len + 4);
    key = (uint8_t *) malloc(key_len * sizeof(uint8_t));
    memcpy(key, local->user_key->buffer, local->user_key->len);
    memintcopy_lt(key + key_len - 4, local->pack_id);
    ++local->pack_id;
    {
        BUFFER_CONSTANT_INSTANCE(_msg, outdata, out_size);
        BUFFER_CONSTANT_INSTANCE(_key, key, key_len);
        ss_md5_hmac_with_key(local->last_client_hash, _msg, _key);
    }
    memcpy(outdata + out_size, local->last_client_hash, 2);
    free(key);
    return out_size + 2;
}

struct buffer_t * auth_chain_a_pack_server_data(struct obfs_t *obfs, const struct buffer_t *buf) {
    struct auth_chain_a_context *local = (struct auth_chain_a_context *) obfs->l_data;
    struct server_info_t *server = &obfs->server;
    struct buffer_t *in_buf = NULL;
    struct buffer_t *data = NULL;
    uint32_t pack_id;
    struct buffer_t *mac_key = NULL;
    uint16_t length = 0;
    uint16_t length2 = 0;

    {
        size_t out_len = 0;
        uint8_t *buffer = (uint8_t *)calloc(buf->len + 4, sizeof(uint8_t));
        ss_encrypt_buffer(local->cipher, local->encrypt_ctx,
            (char*)buf->buffer, (size_t)buf->len, 
            (char *)buffer, &out_len);
        in_buf = buffer_create_from(buffer, out_len);
        free(buffer);
    }

    data = auth_chain_a_rnd_data(obfs, in_buf, &local->random_server, local->last_server_hash);

    pack_id = local->pack_id; // TODO: htonl
    mac_key = buffer_clone(local->user_key);
    buffer_concatenate(mac_key, (uint8_t *)&pack_id, sizeof(uint32_t));

    length2 = *((uint16_t *)(local->last_server_hash + 14)); // TODO: ntohs
    length = ((uint16_t)in_buf->len) ^ length2;

    {
        uint16_t length3 = length; // TODO: htons
        BUFFER_CONSTANT_INSTANCE(other, &length3, sizeof(length3));
        buffer_insert(data, other, 0);
    }
    ss_md5_hmac_with_key(local->last_server_hash, data, mac_key);
    buffer_concatenate(data, local->last_server_hash, 2);

    buffer_free(mac_key);
    buffer_free(in_buf);

    local->pack_id += 1;
    return data;
}

size_t auth_chain_a_pack_auth_data(struct obfs_t *obfs, char *data, size_t datalength, char *outdata) {
    struct server_info_t *server = &obfs->server;
    struct auth_chain_global_data *global = (struct auth_chain_global_data *)obfs->server.g_data;
    struct auth_chain_a_context *local = (struct auth_chain_a_context *) obfs->l_data;

    const int authhead_len = 4 + 8 + 4 + 16 + 4;
    const char* salt = local->salt;
    size_t out_size = authhead_len;
    uint8_t encrypt[20];
    uint8_t key_len;
    uint8_t *key;
    time_t t;
    char password[256] = {0};

    ++global->connection_id;
    if (global->connection_id > 0xFF000000) {
        rand_bytes(global->local_client_id, 8);
        rand_bytes((uint8_t*)(&global->connection_id), 4);
        global->connection_id &= 0xFFFFFF;
    }

    key_len = (uint8_t)(server->iv_len + server->key_len);
    key = (uint8_t *) malloc(key_len * sizeof(uint8_t));
    memcpy(key, server->iv, server->iv_len);
    memcpy(key + server->iv_len, server->key, server->key_len);

    t = time(NULL);
    memintcopy_lt(encrypt, (uint32_t)t);
    memcpy(encrypt + 4, global->local_client_id, 4);
    memintcopy_lt(encrypt + 8, global->connection_id);
    encrypt[12] = (uint8_t)server->overhead;
    encrypt[13] = (uint8_t)(server->overhead >> 8);
    encrypt[14] = 0;
    encrypt[15] = 0;

    // first 12 bytes
    {
        BUFFER_CONSTANT_INSTANCE(_msg, outdata, 4);
        BUFFER_CONSTANT_INSTANCE(_key, key, key_len);
        rand_bytes((uint8_t*)outdata, 4);
        ss_md5_hmac_with_key(local->last_client_hash, _msg, _key);
        memcpy(outdata + 4, local->last_client_hash, 8);
    }

    free(key); key = NULL;

    // uid & 16 bytes auth data
    {
        uint8_t encrypt_data[16];
        size_t enc_key_len;
        uint8_t enc_key[16];
        size_t base64_len;
        size_t salt_len;
        uint8_t encrypt_key_base64[256] = {0};
        int i = 0;
        uint8_t uid[4];
        if (local->user_key->len == 0) {
            if(server->param != NULL && server->param[0] != 0) {
                char *param = server->param;
                char *delim = strchr(param, ':');
                if(delim != NULL) {
                    char uid_str[16] = { 0 };
                    char key_str[128];
                    long uid_long;
                    strncpy(uid_str, param, delim - param);
                    strcpy(key_str, delim + 1);
                    uid_long = strtol(uid_str, NULL, 10);
                    memintcopy_lt((char*)local->uid, (uint32_t)uid_long);

                    buffer_store(local->user_key, (uint8_t *)key_str, strlen(key_str));
                }
            }
            if (local->user_key->len == 0) {
                rand_bytes((uint8_t*)local->uid, 4);
                buffer_store(local->user_key, server->key, server->key_len);
            }
        }
        for (i = 0; i < 4; ++i) {
            uid[i] = (uint8_t)local->uid[i] ^ local->last_client_hash[8 + i];
        }

        std_base64_encode(local->user_key->buffer, (int)local->user_key->len, encrypt_key_base64);
        salt_len = strlen(salt);
        base64_len = (local->user_key->len + 2) / 3 * 4;
        memcpy(encrypt_key_base64 + base64_len, salt, salt_len);

        enc_key_len = base64_len + salt_len;
        bytes_to_key_with_size(encrypt_key_base64, (size_t)enc_key_len, (uint8_t*)enc_key, 16);
        ss_aes_128_cbc_encrypt(16, encrypt, encrypt_data, enc_key);
        memcpy(encrypt, uid, 4);
        memcpy(encrypt + 4, encrypt_data, 16);
    }
    // final HMAC
    {
        BUFFER_CONSTANT_INSTANCE(_msg, encrypt, 20);
        ss_md5_hmac_with_key(local->last_server_hash, _msg, local->user_key);
        memcpy(outdata + 12, encrypt, 20);
        memcpy(outdata + 12 + 20, local->last_server_hash, 4);
    }

    std_base64_encode(local->user_key->buffer, (int)local->user_key->len, (unsigned char *)password);
    std_base64_encode(local->last_client_hash, 16, (unsigned char *)(password + strlen(password)));
    local->cipher = cipher_env_new_instance(password, "rc4");
    local->encrypt_ctx = enc_ctx_new_instance(local->cipher, true);
    local->decrypt_ctx = enc_ctx_new_instance(local->cipher, false);

    out_size += auth_chain_a_pack_client_data(obfs, data, datalength, outdata + out_size);

    return out_size;
}

size_t auth_chain_a_client_pre_encrypt(struct obfs_t *obfs, char **pplaindata, size_t datalength, size_t* capacity) {
    char *plaindata = *pplaindata;
    struct server_info_t *server = (struct server_info_t *)&obfs->server;
    struct auth_chain_a_context *local = (struct auth_chain_a_context*)obfs->l_data;
    char * out_buffer = (char*)malloc((size_t)(datalength * 2 + (SSR_BUFF_SIZE * 2)));
    char * buffer = out_buffer;
    char * data = plaindata;
    size_t len = datalength;
    size_t pack_len;
    size_t unit_size;
    if (len > 0 && local->has_sent_header == 0) {
        size_t head_size = 1200;
        if (head_size > datalength) {
            head_size = datalength;
        }
        pack_len = auth_chain_a_pack_auth_data(obfs, data, head_size, buffer);
        buffer += pack_len;
        data += head_size;
        len -= head_size;
        local->has_sent_header = 1;
    }
    unit_size = server->tcp_mss - server->overhead;
    while ( len > unit_size ) {
        pack_len = auth_chain_a_pack_client_data(obfs, data, unit_size, buffer);
        buffer += pack_len;
        data += unit_size;
        len -= unit_size;
    }
    if (len > 0) {
        pack_len = auth_chain_a_pack_client_data(obfs, data, len, buffer);
        buffer += pack_len;
    }
    len = (size_t)(buffer - out_buffer);
    if ((size_t)*capacity < len) {
        *pplaindata = (char*)realloc(*pplaindata, *capacity = (size_t)(len * 2));
        // TODO check realloc failed
        plaindata = *pplaindata;
    }
    local->last_data_len = (int) datalength;
    memmove(plaindata, out_buffer, len);
    free(out_buffer);
    return len;
}

ssize_t auth_chain_a_client_post_decrypt(struct obfs_t *obfs, char **pplaindata, int datalength, size_t* capacity) {
    int len;
    char *plaindata = *pplaindata;
    struct auth_chain_a_context *local = (struct auth_chain_a_context*)obfs->l_data;
    struct server_info_t *server = (struct server_info_t*)&obfs->server;
    size_t key_len;
    uint8_t *key;
    uint8_t * out_buffer;
    uint8_t * buffer;
    char error = 0;

    if (local->recv_buffer->len + datalength > 16384) {
        return -1;
    }
    buffer_concatenate(local->recv_buffer, plaindata, datalength);

    key_len = local->user_key->len + 4;
    key = (uint8_t*)malloc((size_t)key_len);
    memcpy(key, local->user_key->buffer, local->user_key->len);

    out_buffer = (uint8_t *)malloc((size_t)local->recv_buffer->len);
    buffer = out_buffer;
    while (local->recv_buffer->len > 4) {
        uint8_t hash[16];
        int data_len;
        int rand_len;
        int len;
        unsigned int pos;
        size_t out_len;
        uint8_t *recv_buffer = local->recv_buffer->buffer;

        memintcopy_lt(key + key_len - 4, local->recv_id);

        data_len = (int)(((unsigned)(recv_buffer[1] ^ local->last_server_hash[15]) << 8) + (recv_buffer[0] ^ local->last_server_hash[14]));
        rand_len = (int)get_server_rand_len(local, data_len);
        len = rand_len + data_len;
        if (len >= (SSR_BUFF_SIZE * 2)) {
            local->recv_buffer->len = 0;
            error = 1;
            break;
        }
        if ((len += 4) > local->recv_buffer->len) {
            break;
        }
        {
            BUFFER_CONSTANT_INSTANCE(_msg, recv_buffer, len - 2);
            BUFFER_CONSTANT_INSTANCE(_key, key, key_len);
            ss_md5_hmac_with_key(hash, _msg, _key);
        }
        if (memcmp(hash, recv_buffer + len - 2, 2)) {
            local->recv_buffer->len = 0;
            error = 1;
            break;
        }

        if (data_len > 0 && rand_len > 0) {
            pos = 2 + get_rand_start_pos(rand_len, &local->random_server);
        } else {
            pos = 2;
        }
        ss_decrypt_buffer(local->cipher, local->decrypt_ctx,
                (char*)recv_buffer + pos, (size_t)data_len, (char *)buffer, &out_len);

        if (local->recv_id == 1) {
            server->tcp_mss = (uint16_t)(buffer[0] | (buffer[1] << 8));
            memmove(buffer, buffer + 2, out_len -= 2);
        }
        memcpy(local->last_server_hash, hash, 16);
        ++local->recv_id;
        buffer += out_len;
        buffer_shorten(local->recv_buffer, len, local->recv_buffer->len - len);
    }
    if (error == 0) {
        len = (int)(buffer - out_buffer);
        if ((int)*capacity < len) {
            *pplaindata = (char*)realloc(*pplaindata, *capacity = (size_t)(len * 2));
            plaindata = *pplaindata;
        }
        memmove(plaindata, out_buffer, len);
    } else {
        len = -1;
    }
    free(out_buffer);
    free(key);
    return (ssize_t)len;
}

ssize_t auth_chain_a_client_udp_pre_encrypt(struct obfs_t *obfs, char **pplaindata, size_t datalength, size_t* capacity) {
    char *plaindata = *pplaindata;
    struct server_info_t *server = (struct server_info_t *)&obfs->server;
    struct auth_chain_a_context *local = (struct auth_chain_a_context*)obfs->l_data;
    uint8_t *out_buffer = (uint8_t *) malloc((datalength + (SSR_BUFF_SIZE / 2)) * sizeof(uint8_t));
    uint8_t auth_data[3];
    uint8_t hash[16];
    int rand_len;
    uint8_t *rnd_data;
    size_t outlength;
    char password[256] = {0};
    uint8_t uid[4];
    int i = 0;

    if (local->user_key->len == 0) {
        if(obfs->server.param != NULL && obfs->server.param[0] != 0) {
            char *param = obfs->server.param;
            char *delim = strchr(param, ':');
            if(delim != NULL) {
                char uid_str[16] = { 0 };
                char key_str[128];
                long uid_long;

                strncpy(uid_str, param, delim - param);
                strcpy(key_str, delim + 1);
                uid_long = strtol(uid_str, NULL, 10);
                memintcopy_lt(local->uid, (uint32_t)uid_long);

                buffer_store(local->user_key, (uint8_t *)key_str, strlen(key_str));
            }
        }
        if (local->user_key->len == 0) {
            rand_bytes((uint8_t *)local->uid, 4);
            buffer_store(local->user_key, obfs->server.key, obfs->server.key_len);
        }
    }
    {
        BUFFER_CONSTANT_INSTANCE(_msg, auth_data, 3);
        BUFFER_CONSTANT_INSTANCE(_key, server->key, server->key_len);
        ss_md5_hmac_with_key(hash, _msg, _key);
    }
    rand_len = (int) udp_get_rand_len(&local->random_client, hash);
    rnd_data = (uint8_t *) malloc((size_t)rand_len * sizeof(uint8_t));
    rand_bytes(rnd_data, (int)rand_len);
    outlength = datalength + rand_len + 8;

    std_base64_encode(local->user_key->buffer, (int)local->user_key->len, (unsigned char *)password);
    std_base64_encode(hash, 16, (unsigned char *)(password + strlen(password)));

    {
#if 1
        BUFFER_CONSTANT_INSTANCE(in_obj, plaindata, datalength);
        struct buffer_t *ret = cipher_simple_update_data(password, "rc4", true, in_obj);
        memcpy(out_buffer, ret->buffer, ret->len);
        buffer_free(ret);
#else
        struct cipher_env_t *cipher = cipher_env_new_instance(password, "rc4");
        struct enc_ctx *ctx = enc_ctx_new_instance(cipher, true);
        size_t out_len;
        ss_encrypt_buffer(cipher, ctx,
                plaindata, (size_t)datalength, out_buffer, &out_len);
        enc_ctx_release_instance(cipher, ctx);
        cipher_env_release(cipher);
#endif
    }
    for (i = 0; i < 4; ++i) {
        uid[i] = ((uint8_t)local->uid[i]) ^ hash[i];
    }
    memmove(out_buffer + datalength, rnd_data, rand_len);
    memmove(out_buffer + outlength - 8, auth_data, 3);
    memmove(out_buffer + outlength - 5, uid, 4);
    {
        BUFFER_CONSTANT_INSTANCE(_msg, out_buffer, (int)(outlength - 1));
        ss_md5_hmac_with_key(hash, _msg, local->user_key);
    }
    memmove(out_buffer + outlength - 1, hash, 1);

    if (*capacity < outlength) {
        *pplaindata = (char*)realloc(*pplaindata, *capacity = (outlength * 2));
        plaindata = *pplaindata;
    }
    memmove(plaindata, out_buffer, outlength);

    free(out_buffer);
    free(rnd_data);

    return (ssize_t)outlength;
}

ssize_t auth_chain_a_client_udp_post_decrypt(struct obfs_t *obfs, char **pplaindata, size_t datalength, size_t* capacity) {
    char *plaindata = *pplaindata;
    struct server_info_t *server = (struct server_info_t *)&obfs->server;
    struct auth_chain_a_context *local = (struct auth_chain_a_context*)obfs->l_data;
    uint8_t hash[16];
    int rand_len;
    size_t outlength;
    char password[256] = {0};

    if (datalength <= 8) {
        return 0;
    }

    {
        BUFFER_CONSTANT_INSTANCE(_msg, plaindata, (int)(datalength - 1));
        ss_md5_hmac_with_key(hash, _msg, local->user_key);
    }
    if (*hash != ((uint8_t*)plaindata)[datalength - 1]) {
        return 0;
    }
    {
        BUFFER_CONSTANT_INSTANCE(_msg, plaindata + datalength - 8, 7);
        BUFFER_CONSTANT_INSTANCE(_key, server->key, server->key_len);
        ss_md5_hmac_with_key(hash, _msg, _key);
    }
    rand_len = (int)udp_get_rand_len(&local->random_server, hash);
    outlength = datalength - rand_len - 8;

    std_base64_encode(local->user_key->buffer, (int)local->user_key->len, (unsigned char *)password);
    std_base64_encode(hash, 16, (unsigned char *)(password + strlen(password)));

    {
#if 1
        BUFFER_CONSTANT_INSTANCE(in_obj, plaindata, outlength);
        struct buffer_t *ret = cipher_simple_update_data(password, "rc4", false, in_obj);
        memcpy(plaindata, ret->buffer, ret->len);
        buffer_free(ret);
#else
        struct cipher_env_t *cipher = cipher_env_new_instance(password, "rc4");
        struct enc_ctx *ctx = enc_ctx_new_instance(cipher, false);
        size_t out_len;
        ss_decrypt_buffer(cipher, ctx,
                plaindata, (size_t)outlength, plaindata, &out_len);
        enc_ctx_release_instance(cipher, ctx);
        cipher_env_release(cipher);
#endif
    }

    return (ssize_t)outlength;
}

struct buffer_t * auth_chain_a_server_pre_encrypt(struct obfs_t *obfs, const struct buffer_t *buf) {
    struct server_info_t *server = (struct server_info_t *)&obfs->server;
    struct auth_chain_a_context *local = (struct auth_chain_a_context*)obfs->l_data;
    struct buffer_t *tmp_buf = NULL;
    struct buffer_t *swap = NULL;
    struct buffer_t *ret = buffer_alloc(SSR_BUFF_SIZE);
    if (local->pack_id == 1) {
        uint16_t tcp_mss = server->tcp_mss; // TODO: htons
        tmp_buf = buffer_create_from((const uint8_t *)&tcp_mss, sizeof(uint16_t));
        buffer_concatenate2(tmp_buf, buf);
        local->unit_len = server->tcp_mss - local->client_over_head;
    } else {
        tmp_buf = buffer_clone(buf);
    }
    while (tmp_buf->len > local->unit_len) {
        BUFFER_CONSTANT_INSTANCE(iter, tmp_buf->buffer, local->unit_len);

        swap = auth_chain_a_pack_server_data(obfs, iter);
        buffer_concatenate2(ret, swap);
        buffer_free(swap);

        buffer_shorten(tmp_buf, local->unit_len, tmp_buf->len - local->unit_len);
    }
    swap = auth_chain_a_pack_server_data(obfs, tmp_buf);
    buffer_concatenate2(ret, swap);
    buffer_free(swap);

    buffer_free(tmp_buf);

    return ret;
}

struct buffer_t * auth_chain_a_server_post_decrypt(struct obfs_t *obfs, struct buffer_t *buf, bool *need_feedback) {
    struct server_info_t *server = (struct server_info_t *)&obfs->server;
    struct auth_chain_a_context *local = (struct auth_chain_a_context*)obfs->l_data;
    struct buffer_t *out_buf = buffer_alloc(SSR_BUFF_SIZE);
    struct buffer_t *mac_key2 = NULL;

    if (need_feedback) { *need_feedback = false; }

    buffer_concatenate2(local->recv_buffer, buf);

    if (local->has_recv_header == false) {
        uint8_t md5data[16 + 1] = { 0 };
        size_t len = local->recv_buffer->len;
        uint32_t uid = 0;
        uint8_t head[16 + 1] = { 0 };
        uint32_t utc_time = 0;
        uint32_t client_id = 0;
        uint32_t connection_id = 0;
        int time_diff;
        uint8_t *password = NULL;

        if (len>=12 || len==7 || len==8) {
            size_t recv_len = min(len, 12);
            struct buffer_t *mac_key = buffer_create_from(server->recv_iv, server->recv_iv_len);
            buffer_concatenate(mac_key, server->key, server->key_len);
            {
                BUFFER_CONSTANT_INSTANCE(_msg, local->recv_buffer->buffer, 4);
                ss_md5_hmac_with_key(md5data, _msg, mac_key);
            }
            buffer_free(mac_key);
            if (memcmp(md5data, local->recv_buffer->buffer+4, recv_len-4) != 0) {
                return out_buf;
            }
        }
        if (local->recv_buffer->len < (12 + 24)) {
            return out_buf;
        }

        memmove(local->last_client_hash, md5data, 16);
        uid = *((uint32_t *)(local->recv_buffer->buffer + 12)); // TODO: ntohl
        uid = uid ^ (*((uint32_t *)(md5data + 8))); // TODO: ntohl
        local->user_id_num = uid;

        buffer_store(local->user_key, server->key, server->key_len);

        {
            BUFFER_CONSTANT_INSTANCE(_msg, local->recv_buffer->buffer + 12, 20);
            ss_md5_hmac_with_key(md5data, _msg, local->user_key);
        }
        if (memcmp(md5data, local->recv_buffer->buffer+32, 4) != 0) {
            // logging.error('%s data incorrect auth HMAC-MD5 from %s:%d, data %s' % (self.no_compatible_method, self.server_info.client, self.server_info.client_port, binascii.hexlify(self.recv_buf)))
            return out_buf;
        }

        memcpy(local->last_server_hash, md5data, 16);
        {
            uint8_t enc_key[16 + 1] = { 0 };
            size_t b64len = (size_t) std_base64_encode_len((int) local->user_key->len);
            size_t salt_len = strlen(local->salt);
            uint8_t *key = (uint8_t *)calloc(b64len + salt_len, sizeof(uint8_t));
            std_base64_encode(local->user_key->buffer, (int)local->user_key->len, key);
            strcat((char *)key, local->salt);
            bytes_to_key_with_size(key, strlen((char *)key), enc_key, 16);
            ss_aes_128_cbc_decrypt(16, local->recv_buffer->buffer+16, head, enc_key);
            free(key);
        }
        local->client_over_head = (uint16_t) (*((uint16_t *)(head + 12))); // TODO: ntohs

        utc_time = (uint32_t) (*((uint32_t *)(head + 0))); // TODO: ntohl
        client_id = (uint32_t) (*((uint32_t *)(head + 4))); // TODO: ntohl
        connection_id = (uint32_t) (*((uint32_t *)(head + 8))); // TODO: ntohl

        time_diff = abs((int)time(NULL) - (int)utc_time);
        if (time_diff > local->max_time_dif) {
            // logging.info('%s: wrong timestamp, time_dif %d, data %s' % (self.no_compatible_method, time_dif, binascii.hexlify(head)))
            return out_buf;
        }

        local->client_id = client_id;
        local->connection_id = connection_id;
        {
            size_t b64len1 = (size_t) std_base64_encode_len((int) local->user_key->len);
            size_t b64len2 = (size_t) std_base64_encode_len((int) sizeof(local->last_client_hash));
            password = (uint8_t *)calloc(b64len1 + b64len2, sizeof(uint8_t));
            b64len1 = std_base64_encode(local->user_key->buffer, (int)local->user_key->len, password);
            b64len2 = std_base64_encode(local->last_client_hash, (int)sizeof(local->last_client_hash), password + b64len1);
        }
        buffer_shorten(local->recv_buffer, 36, local->recv_buffer->len - 36);
        local->has_recv_header = true;
        if (need_feedback) { *need_feedback = true; }

        assert(local->cipher == NULL);
        local->cipher = cipher_env_new_instance((char *)password, "rc4");
        local->encrypt_ctx = enc_ctx_new_instance(local->cipher, true);
        local->decrypt_ctx = enc_ctx_new_instance(local->cipher, false);
        free(password);
    }

    mac_key2 = buffer_alloc(SSR_BUFF_SIZE);

    while (local->recv_buffer->len) {
        uint16_t data_len = 0;
        size_t rand_len = 0;
        size_t length = 0;
        uint8_t client_hash[16 + 1] = { 0 };
        size_t pos = 0;
        buffer_replace(mac_key2, local->user_key);
        buffer_concatenate(mac_key2, (uint8_t *)&local->recv_id, 4); // TODO: htonl(local->recv_id);

        data_len = *((uint16_t *)local->recv_buffer->buffer); // TODO: ntohs
        data_len = data_len ^ (*((uint16_t *)(local->last_client_hash + 14))); // TODO: ntohs

        rand_len = local->get_tcp_rand_len(local, data_len, &local->random_client, local->last_client_hash);
        length = data_len + rand_len;
        if (length >= 4096) {
            // logging.info(self.no_compatible_method + ': over size')
            buffer_reset(local->recv_buffer);
            if (local->recv_id == 0) {
                buffer_reset(out_buf);
            } else {
                buffer_free(out_buf); out_buf = NULL;
            }
            break;
        }
        if (length + 4 > local->recv_buffer->len) {
            break;
        }
        {
            BUFFER_CONSTANT_INSTANCE(_msg, local->recv_buffer->buffer, length + 2);
            ss_md5_hmac_with_key(client_hash, _msg, mac_key2);
        }
        if (memcmp(client_hash, local->recv_buffer->buffer+length+2, 2) != 0) {
            // logging.info('%s: checksum error, data %s' % (self.no_compatible_method, binascii.hexlify(self.recv_buf[:length])))
            buffer_reset(local->recv_buffer);
            if (local->recv_id == 0) {
                buffer_reset(out_buf);
            } else {
                buffer_free(out_buf); out_buf = NULL;
            }
            break;
        }

        local->recv_id += 1;

        if (data_len > 0 && rand_len > 0) {
            pos = 2 + get_rand_start_pos((int)rand_len, &local->random_client);
        } else {
            pos = 2;
        }

        {
            size_t out_len = 0;
            char *buffer = (char *)calloc(local->recv_buffer->len, sizeof(char));
            ss_decrypt_buffer(local->cipher, local->decrypt_ctx,
                (char*)local->recv_buffer->buffer + pos, (size_t)data_len, 
                (char *)buffer, &out_len);
            buffer_concatenate(out_buf, (uint8_t *)buffer, out_len);
            free(buffer);
        }
        memcpy(local->last_client_hash, client_hash, 16);
        buffer_shorten(local->recv_buffer, length + 4, local->recv_buffer->len - (length + 4));

        if (data_len == 0) {
            if (need_feedback) { *need_feedback = true; }
        }
    }
    buffer_free(mac_key2);
    return out_buf;
}


//============================= auth_chain_b ==================================

unsigned int auth_chain_b_get_rand_len(struct auth_chain_a_context *local, int datalength, struct shift128plus_ctx *random, const uint8_t last_hash[16]);
void auth_chain_b_set_server_info(struct obfs_t *obfs, struct server_info_t *server);
void auth_chain_b_dispose(struct obfs_t *obfs);

void auth_chain_b_new_obfs(struct obfs_t *obfs) {
    struct auth_chain_a_context *auth_chain_a = NULL;

    auth_chain_a_new_obfs(obfs);
    auth_chain_a = (struct auth_chain_a_context *) obfs->l_data;

    auth_chain_a->salt = "auth_chain_b";
    auth_chain_a->get_tcp_rand_len = auth_chain_b_get_rand_len;
    auth_chain_a->subclass_context = calloc(1, sizeof(struct auth_chain_b_context));

    obfs->set_server_info = auth_chain_b_set_server_info;
    obfs->dispose = auth_chain_b_dispose;
}

void auth_chain_b_dispose(struct obfs_t *obfs) {
    struct auth_chain_a_context *auth_chain_a = (struct auth_chain_a_context *)obfs->l_data;
    struct auth_chain_b_context *auth_chain_b = (struct auth_chain_b_context *)auth_chain_a->subclass_context;
    auth_chain_a->subclass_context = NULL;
    if (auth_chain_b != NULL) {
        if (auth_chain_b->data_size_list != NULL) {
            free(auth_chain_b->data_size_list);
            auth_chain_b->data_size_list = NULL;
            auth_chain_b->data_size_list_length = 0;
        }
        if (auth_chain_b->data_size_list2 != NULL) {
            free(auth_chain_b->data_size_list2);
            auth_chain_b->data_size_list2 = NULL;
            auth_chain_b->data_size_list2_length = 0;
        }
        free(auth_chain_b);
    }
    auth_chain_a_dispose(obfs);
}

void auth_chain_b_init_data_size(struct obfs_t *obfs) {
    size_t i = 0;
    struct server_info_t *server = &obfs->server;
    struct auth_chain_a_context *auth_chain_a = (struct auth_chain_a_context *)obfs->l_data;
    struct auth_chain_b_context *auth_chain_b = (struct auth_chain_b_context *)auth_chain_a->subclass_context;

    struct shift128plus_ctx *random = (struct shift128plus_ctx *) calloc(1, sizeof(struct shift128plus_ctx));

    shift128plus_init_from_bin(random, server->key, 16);
    auth_chain_b->data_size_list_length = shift128plus_next(random) % 8 + 4;
    auth_chain_b->data_size_list = (int *)calloc(auth_chain_b->data_size_list_length, sizeof(auth_chain_b->data_size_list[0]));
    for (i = 0; i < auth_chain_b->data_size_list_length; i++) {
        auth_chain_b->data_size_list[i] = shift128plus_next(random) % 2340 % 2040 % 1440;
    }
    // stdlib qsort
    qsort(auth_chain_b->data_size_list,
        auth_chain_b->data_size_list_length,
        sizeof(auth_chain_b->data_size_list[0]),
        data_size_list_compare
        );

    auth_chain_b->data_size_list2_length = shift128plus_next(random) % 16 + 8;
    auth_chain_b->data_size_list2 = (int *)calloc(auth_chain_b->data_size_list2_length, sizeof(auth_chain_b->data_size_list2[0]));
    for (i = 0; i < auth_chain_b->data_size_list2_length; i++) {
        auth_chain_b->data_size_list2[i] = shift128plus_next(random) % 2340 % 2040 % 1440;
    }
    // stdlib qsort
    qsort(auth_chain_b->data_size_list2,
        auth_chain_b->data_size_list2_length,
        sizeof(auth_chain_b->data_size_list2[0]),
        data_size_list_compare
        );

    free(random);
}

void auth_chain_b_set_server_info(struct obfs_t *obfs, struct server_info_t *server) {
    auth_chain_a_set_server_info(obfs, server);
    auth_chain_b_init_data_size(obfs);
}

unsigned int auth_chain_b_get_rand_len(struct auth_chain_a_context *local, int datalength, struct shift128plus_ctx *random, const uint8_t last_hash[16]) {
    struct server_info_t *server = &local->obfs->server;
    uint16_t overhead = server->overhead;
    struct auth_chain_b_context *auth_chain_b = (struct auth_chain_b_context *)local->subclass_context;
    size_t pos;
    size_t final_pos;
    size_t pos2;
    size_t final_pos2;

    if (datalength >= 1440) {
        return 0;
    }

    shift128plus_init_from_bin_datalen(random, last_hash, 16, datalength);

    pos = auth_chain_find_pos(auth_chain_b->data_size_list, auth_chain_b->data_size_list_length, datalength + overhead);
    final_pos = pos + shift128plus_next(random) % auth_chain_b->data_size_list_length;
    if (final_pos < auth_chain_b->data_size_list_length) {
        return auth_chain_b->data_size_list[final_pos] - datalength - overhead;
    }

    pos2 = auth_chain_find_pos(auth_chain_b->data_size_list2, auth_chain_b->data_size_list2_length, datalength + overhead);
    final_pos2 = pos2 + shift128plus_next(random) % auth_chain_b->data_size_list2_length;
    if (final_pos2 < auth_chain_b->data_size_list2_length) {
        return auth_chain_b->data_size_list2[final_pos2] - datalength - overhead;
    }
    if (final_pos2 < pos2 + auth_chain_b->data_size_list2_length - 1) {
        return 0;
    }

    if (datalength > 1300) {
        return shift128plus_next(random) % 31;
    }
    if (datalength > 900) {
        return shift128plus_next(random) % 127;
    }
    if (datalength > 400) {
        return shift128plus_next(random) % 521;
    }
    return shift128plus_next(random) % 1021;
}


//============================= auth_chain_c ==================================

unsigned int auth_chain_c_get_rand_len(struct auth_chain_a_context *local, int datalength, struct shift128plus_ctx *random, const uint8_t last_hash[16]);
void auth_chain_c_set_server_info(struct obfs_t *obfs, struct server_info_t *server);
void auth_chain_c_dispose(struct obfs_t *obfs);

void auth_chain_c_new_obfs(struct obfs_t *obfs) {
    struct auth_chain_a_context *auth_chain_a = NULL;

    auth_chain_a_new_obfs(obfs);
    auth_chain_a = (struct auth_chain_a_context *) obfs->l_data;

    auth_chain_a->salt = "auth_chain_c";
    auth_chain_a->get_tcp_rand_len = auth_chain_c_get_rand_len;
    auth_chain_a->subclass_context = calloc(1, sizeof(struct auth_chain_c_context));

    obfs->set_server_info = auth_chain_c_set_server_info;
    obfs->dispose = auth_chain_c_dispose;
}

void auth_chain_c_dispose(struct obfs_t *obfs) {
    struct auth_chain_a_context *auth_chain_a = (struct auth_chain_a_context *)obfs->l_data;
    struct auth_chain_c_context *auth_chain_c = (struct auth_chain_c_context *)auth_chain_a->subclass_context;
    auth_chain_a->subclass_context = NULL;
    if (auth_chain_c != NULL) {
        if (auth_chain_c->data_size_list0 != NULL) {
            free(auth_chain_c->data_size_list0);
            auth_chain_c->data_size_list0 = NULL;
            auth_chain_c->data_size_list0_length = 0;
        }
        free(auth_chain_c);
    }
    auth_chain_a_dispose(obfs);
}

void auth_chain_c_init_data_size(struct obfs_t *obfs) {
    size_t i = 0;
    struct server_info_t *server = &obfs->server;

    struct auth_chain_a_context *auth_chain_a = (struct auth_chain_a_context *)obfs->l_data;
    struct auth_chain_c_context *auth_chain_c = (struct auth_chain_c_context *)auth_chain_a->subclass_context;

    struct shift128plus_ctx *random = (struct shift128plus_ctx *) calloc(1, sizeof(struct shift128plus_ctx));

    shift128plus_init_from_bin(random, server->key, 16);
    auth_chain_c->data_size_list0_length = shift128plus_next(random) % (8 + 16) + (4 + 8);
    auth_chain_c->data_size_list0 = (int *)malloc(auth_chain_c->data_size_list0_length * sizeof(int));
    for (i = 0; i < auth_chain_c->data_size_list0_length; i++) {
        auth_chain_c->data_size_list0[i] = shift128plus_next(random) % 2340 % 2040 % 1440;
    }
    // stdlib qsort
    qsort(auth_chain_c->data_size_list0,
        auth_chain_c->data_size_list0_length,
        sizeof(int),
        data_size_list_compare
        );

    free(random);
}

void auth_chain_c_set_server_info(struct obfs_t *obfs, struct server_info_t *server) {
    auth_chain_a_set_server_info(obfs, server);
    auth_chain_c_init_data_size(obfs);
}

unsigned int auth_chain_c_get_rand_len(struct auth_chain_a_context *local, int datalength, struct shift128plus_ctx *random, const uint8_t last_hash[16]) {
    struct server_info_t *server = &local->obfs->server;
    uint16_t overhead = server->overhead;
    struct auth_chain_c_context *auth_chain_c = (struct auth_chain_c_context *)local->subclass_context;
    int other_data_size = datalength + overhead;
    size_t pos;
    size_t final_pos;

    // must init random in here to make sure output sync in server and client
    shift128plus_init_from_bin_datalen(random, last_hash, 16, datalength);

    if (other_data_size >= auth_chain_c->data_size_list0[auth_chain_c->data_size_list0_length - 1]) {
        if (datalength > 1440)
            return 0;
        if (datalength > 1300)
            return shift128plus_next(random) % 31;
        if (datalength > 900)
            return shift128plus_next(random) % 127;
        if (datalength > 400)
            return shift128plus_next(random) % 521;
        return shift128plus_next(random) % 1021;
    }

    pos = auth_chain_find_pos(auth_chain_c->data_size_list0, auth_chain_c->data_size_list0_length, other_data_size);
    // random select a size in the leftover data_size_list0
    final_pos = pos + shift128plus_next(random) % (auth_chain_c->data_size_list0_length - pos);
    return auth_chain_c->data_size_list0[final_pos] - other_data_size;
}

//============================= auth_chain_d ==================================

unsigned int auth_chain_d_get_rand_len(struct auth_chain_a_context *local, int datalength, struct shift128plus_ctx *random, const uint8_t last_hash[16]);
void auth_chain_d_set_server_info(struct obfs_t *obfs, struct server_info_t *server);

void auth_chain_d_new_obfs(struct obfs_t *obfs) {
    struct auth_chain_a_context *auth_chain_a = NULL;

    auth_chain_c_new_obfs(obfs);
    auth_chain_a = (struct auth_chain_a_context *) obfs->l_data;

    auth_chain_a->salt = "auth_chain_d";
    auth_chain_a->get_tcp_rand_len = auth_chain_d_get_rand_len;

    obfs->set_server_info = auth_chain_d_set_server_info;
}

#define AUTH_CHAIN_D_MAX_DATA_SIZE_LIST_LIMIT_SIZE 64

void auth_chain_d_check_and_patch_data_size(struct obfs_t *obfs, struct shift128plus_ctx *random) {
    struct auth_chain_a_context *auth_chain_a = (struct auth_chain_a_context *)obfs->l_data;
    struct auth_chain_c_context *auth_chain_c = (struct auth_chain_c_context *)auth_chain_a->subclass_context;

    while (auth_chain_c->data_size_list0[auth_chain_c->data_size_list0_length - 1] < 1300 &&
        auth_chain_c->data_size_list0_length < AUTH_CHAIN_D_MAX_DATA_SIZE_LIST_LIMIT_SIZE)
    {
        uint64_t data = shift128plus_next(random) % 2340 % 2040 % 1440;
        auth_chain_c->data_size_list0[auth_chain_c->data_size_list0_length] = (int) data;

        ++auth_chain_c->data_size_list0_length;
    }
}

void auth_chain_d_init_data_size(struct obfs_t *obfs) {
    size_t i = 0;
    size_t old_len;
    struct server_info_t *server = &obfs->server;

    struct auth_chain_a_context *auth_chain_a = (struct auth_chain_a_context *)obfs->l_data;
    struct auth_chain_c_context *auth_chain_c = (struct auth_chain_c_context *)auth_chain_a->subclass_context;

    struct shift128plus_ctx *random = (struct shift128plus_ctx *)calloc(1, sizeof(struct shift128plus_ctx));

    shift128plus_init_from_bin(random, server->key, 16);
    auth_chain_c->data_size_list0_length = shift128plus_next(random) % (8 + 16) + (4 + 8);
    auth_chain_c->data_size_list0 = (int *)malloc(AUTH_CHAIN_D_MAX_DATA_SIZE_LIST_LIMIT_SIZE * sizeof(int));
    for (i = 0; i < auth_chain_c->data_size_list0_length; i++) {
        auth_chain_c->data_size_list0[i] = shift128plus_next(random) % 2340 % 2040 % 1440;
    }
    // stdlib qsort
    qsort(auth_chain_c->data_size_list0, auth_chain_c->data_size_list0_length,
        sizeof(auth_chain_c->data_size_list0[0]), data_size_list_compare);

    old_len = auth_chain_c->data_size_list0_length;
    auth_chain_d_check_and_patch_data_size(obfs, random);
    if (old_len != auth_chain_c->data_size_list0_length) {
        // if check_and_patch_data_size are work, re-sort again.
        // stdlib qsort
        qsort(auth_chain_c->data_size_list0, auth_chain_c->data_size_list0_length,
            sizeof(auth_chain_c->data_size_list0[0]), data_size_list_compare);
    }

    free(random);
}

void auth_chain_d_set_server_info(struct obfs_t *obfs, struct server_info_t *server) {
    auth_chain_a_set_server_info(obfs, server);
    auth_chain_d_init_data_size(obfs);
}

unsigned int auth_chain_d_get_rand_len(struct auth_chain_a_context *local, int datalength, struct shift128plus_ctx *random, const uint8_t last_hash[16]) {
    struct server_info_t *server = &local->obfs->server;
    size_t pos;
    size_t final_pos;

    uint16_t overhead = server->overhead;
    struct auth_chain_c_context *auth_chain_c = (struct auth_chain_c_context *)local->subclass_context;

    int other_data_size = datalength + overhead;

    // if other_data_size > the bigest item in data_size_list0, not padding any data
    if (other_data_size >= auth_chain_c->data_size_list0[auth_chain_c->data_size_list0_length - 1]) {
        return 0;
    }

    shift128plus_init_from_bin_datalen(random, last_hash, 16, datalength);
    pos = auth_chain_find_pos(auth_chain_c->data_size_list0, auth_chain_c->data_size_list0_length, other_data_size);
    // random select a size in the leftover data_size_list0
    final_pos = pos + shift128plus_next(random) % (auth_chain_c->data_size_list0_length - pos);
    return auth_chain_c->data_size_list0[final_pos] - other_data_size;
}


//============================= auth_chain_e ==================================

unsigned int auth_chain_e_get_rand_len(struct auth_chain_a_context *local, int datalength, struct shift128plus_ctx *random, const uint8_t last_hash[16]);

void auth_chain_e_new_obfs(struct obfs_t *obfs) {
    struct auth_chain_a_context *auth_chain_a = NULL;

    auth_chain_d_new_obfs(obfs);
    auth_chain_a = (struct auth_chain_a_context *)obfs->l_data;

    auth_chain_a->salt = "auth_chain_e";
    auth_chain_a->get_tcp_rand_len = auth_chain_e_get_rand_len;
}

unsigned int auth_chain_e_get_rand_len(struct auth_chain_a_context *local, int datalength, struct shift128plus_ctx *random, const uint8_t last_hash[16]) {
    struct server_info_t *server;
    uint16_t overhead;
    struct auth_chain_c_context *auth_chain_c;
    int other_data_size;
    size_t pos;

    shift128plus_init_from_bin_datalen(random, last_hash, 16, datalength);

    server = &local->obfs->server;

    overhead = server->overhead;
    auth_chain_c = (struct auth_chain_c_context *)local->subclass_context;

    other_data_size = datalength + overhead;

    // if other_data_size > the bigest item in data_size_list0, not padding any data
    if (other_data_size >= auth_chain_c->data_size_list0[auth_chain_c->data_size_list0_length - 1]) {
        return 0;
    }

    // use the mini size in the data_size_list0
    pos = auth_chain_find_pos(auth_chain_c->data_size_list0, auth_chain_c->data_size_list0_length, other_data_size);
    return auth_chain_c->data_size_list0[pos] - other_data_size;
}


//============================= auth_chain_f ==================================

void auth_chain_f_set_server_info(struct obfs_t *obfs, struct server_info_t *server);

void auth_chain_f_new_obfs(struct obfs_t *obfs) {
    struct auth_chain_a_context *auth_chain_a = NULL;

    auth_chain_e_new_obfs(obfs);
    auth_chain_a = (struct auth_chain_a_context *)obfs->l_data;

    auth_chain_a->salt = "auth_chain_f";

    obfs->set_server_info = auth_chain_f_set_server_info;
}

static void auth_chain_f_init_data_size(struct obfs_t *obfs, const uint8_t *key_change_datetime_key_bytes) {
    size_t i = 0;
    struct server_info_t *server = &obfs->server;
    struct auth_chain_a_context *auth_chain_a = (struct auth_chain_a_context *)obfs->l_data;
    struct auth_chain_c_context *auth_chain_c = (struct auth_chain_c_context *)auth_chain_a->subclass_context;
    struct shift128plus_ctx *random = (struct shift128plus_ctx *)malloc(sizeof(struct shift128plus_ctx));
    uint8_t *newKey = (uint8_t *)malloc(sizeof(uint8_t) * server->key_len);
    size_t len;
    size_t old_len;

    memcpy(newKey, server->key, server->key_len);
    for (i = 0; i != 8; ++i) {
        newKey[i] ^= key_change_datetime_key_bytes[i];
    }
    shift128plus_init_from_bin(random, newKey, server->key_len);
    free(newKey);
    newKey = NULL;

    auth_chain_c->data_size_list0_length = shift128plus_next(random) % (8 + 16) + (4 + 8);
    len = max(AUTH_CHAIN_D_MAX_DATA_SIZE_LIST_LIMIT_SIZE, auth_chain_c->data_size_list0_length);
    auth_chain_c->data_size_list0 = (int *) calloc(len, sizeof(auth_chain_c->data_size_list0[0]));
    for (i = 0; i < auth_chain_c->data_size_list0_length; i++) {
        auth_chain_c->data_size_list0[i] = shift128plus_next(random) % 2340 % 2040 % 1440;
    }
    // stdlib qsort
    qsort(auth_chain_c->data_size_list0,
        auth_chain_c->data_size_list0_length,
        sizeof(auth_chain_c->data_size_list0[0]),
        data_size_list_compare
        );

    old_len = auth_chain_c->data_size_list0_length;
    auth_chain_d_check_and_patch_data_size(obfs, random);
    if (old_len != auth_chain_c->data_size_list0_length) {
        // if check_and_patch_data_size are work, re-sort again.
        // stdlib qsort
        qsort(auth_chain_c->data_size_list0,
            auth_chain_c->data_size_list0_length,
            sizeof(auth_chain_c->data_size_list0[0]),
            data_size_list_compare
            );
    }

    free(random);
}

void auth_chain_f_set_server_info(struct obfs_t *obfs, struct server_info_t *server) {
    uint64_t key_change_interval = 60 * 60 * 24;     // a day by second
    uint8_t *key_change_datetime_key_bytes;
    uint64_t key_change_datetime_key;
    int i = 0;

    set_server_info(obfs, server);
    if (server->param != NULL && server->param[0] != 0) {
        char *delim1 = strchr(server->param, '#');
        if (delim1 != NULL && delim1[1] != '\0') {
            char *delim2;
            size_t l;

            ++delim1;
            delim2 = strchr(delim1, '#');
            if (delim2 == NULL) {
                delim2 = strchr(delim1, '\0');
            }
            l = delim2 - delim1;
            if (l > 2) {
                long long n = strtoll(delim1, &delim2, 0);
                if (n != 0 && n != LLONG_MAX && n != LLONG_MIN && n > 0) {
                    key_change_interval = (uint64_t)n;
                }
            }
        }
    }

    key_change_datetime_key_bytes = (uint8_t *)malloc(sizeof(uint8_t) * 8);
    key_change_datetime_key = (uint64_t)(time(NULL)) / key_change_interval;
    for (i = 7; i >= 0; --i) {
        key_change_datetime_key_bytes[7 - i] = (uint8_t)((key_change_datetime_key >> (8 * i)) & 0xFF);
    }

    auth_chain_f_init_data_size(obfs, key_change_datetime_key_bytes);

    free(key_change_datetime_key_bytes);
    key_change_datetime_key_bytes = NULL;
}
