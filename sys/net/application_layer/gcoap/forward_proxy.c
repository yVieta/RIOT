/*
 * Copyright (C) 2020 HAW Hamburg
 * Copyright (C) 2022 Freie Universität Berlin
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @{
 *
 * @file
 * @author  Cenk Gündoğan <cenk.guendogan@haw-hamburg.de>
 * @author  Martine S. Lenders <m.lenders@fu-berlin.de>
 */

#include <stdbool.h>

#include "net/gcoap.h"
#include "net/gcoap/forward_proxy.h"
#include "uri_parser.h"
#include "net/nanocoap/cache.h"

#define ENABLE_DEBUG    0
#include "debug.h"

typedef struct {
    bool in_use;
    bool validating;
    sock_udp_ep_t ep;
#if IS_USED(MODULE_NANOCOAP_CACHE)
    uint8_t cache_key[CONFIG_NANOCOAP_CACHE_KEY_LENGTH];
#endif
} client_ep_t;

static uint8_t proxy_req_buf[CONFIG_GCOAP_PDU_BUF_SIZE];
static client_ep_t _client_eps[CONFIG_GCOAP_REQ_WAITING_MAX];

static int _request_matcher_forward_proxy(gcoap_listener_t *listener,
                                          const coap_resource_t **resource,
                                          coap_pkt_t *pdu);
static ssize_t _forward_proxy_handler(coap_pkt_t* pdu, uint8_t *buf,
                                      size_t len, void *ctx);

const coap_resource_t forward_proxy_resources[] = {
    { "/", COAP_IGNORE, _forward_proxy_handler, NULL },
};

gcoap_listener_t forward_proxy_listener = {
    &forward_proxy_resources[0],
    ARRAY_SIZE(forward_proxy_resources),
    GCOAP_SOCKET_TYPE_UDP,
    NULL,
    NULL,
    _request_matcher_forward_proxy
};

void gcoap_forward_proxy_init(void)
{
    gcoap_register_listener(&forward_proxy_listener);

    /* initialize the nanocoap cache operation, if compiled */
    if (IS_USED(MODULE_NANOCOAP_CACHE)) {
        nanocoap_cache_init();
    }
}

static int _cache_build_response(nanocoap_cache_entry_t *ce,
                                 coap_pkt_t *pdu,
                                 uint8_t *buf,
                                 size_t len)
{
    if ((pdu->hdr->code == COAP_METHOD_GET) || (pdu->hdr->code == COAP_METHOD_FETCH)) {
        uint8_t *req_etag;
        /* Searching for more ETags might become necessary in the future */
        ssize_t req_etag_len = coap_opt_get_opaque(pdu, COAP_OPT_ETAG, &req_etag);

        if (req_etag_len > 0) {
            /* ETag found, validate from cache entry */
            uint8_t *cache_etag;
            ssize_t cache_etag_len = coap_opt_get_opaque(&ce->response_pkt, COAP_OPT_ETAG,
                                                         &cache_etag);

            if ((cache_etag_len == req_etag_len) &&
                (memcmp(req_etag, cache_etag, req_etag_len) == 0)) {
                gcoap_resp_init(pdu, buf, len, COAP_CODE_VALID);
                coap_opt_add_opaque(pdu, COAP_OPT_ETAG, req_etag, req_etag_len);
                return coap_get_total_hdr_len(pdu);
            }
        }
    }
    /* Use the same code from the cached content. Use other header
     * fields from the incoming request */
    gcoap_resp_init(pdu, buf, len, ce->response_pkt.hdr->code);
    /* copy all options and possible payload from the cached response
     * to the new response */
    unsigned header_len_req = coap_get_total_hdr_len(pdu);
    unsigned header_len_cached = coap_get_total_hdr_len(&ce->response_pkt);
    unsigned opt_payload_len = ce->response_len - header_len_cached;

    memcpy((buf + header_len_req),
           (ce->response_buf + header_len_cached),
           opt_payload_len);
    return header_len_req + opt_payload_len;
}

static int _cache_lookup_and_process(coap_pkt_t *pdu,
                                     uint8_t *buf,
                                     size_t len,
                                     client_ep_t *cep,
                                     nanocoap_cache_entry_t **ce)
{
    (void) cep;

    uint8_t cache_key[SHA256_DIGEST_LENGTH];
    ztimer_now_t now = ztimer_now(ZTIMER_SEC);
    nanocoap_cache_key_generate(pdu, cache_key);
    *ce = nanocoap_cache_key_lookup(cache_key);

    /* cache hit, methods are equal, and cache entry is not stale */
    if (*ce &&
        ((*ce)->request_method == coap_get_code(pdu)) &&
        ((*ce)->max_age > now)) {
        /* use response from cache */
        return _cache_build_response(*ce, pdu, buf, len);
    }

#if IS_USED(MODULE_NANOCOAP_CACHE)
    memcpy(cep->cache_key, cache_key, CONFIG_NANOCOAP_CACHE_KEY_LENGTH);
#endif

    return 0;
}

static client_ep_t *_allocate_client_ep(sock_udp_ep_t *ep)
{
    client_ep_t *cep;
    for (cep = _client_eps;
         cep < (_client_eps + CONFIG_GCOAP_REQ_WAITING_MAX);
         cep++) {
        if (!cep->in_use) {
            cep->in_use = true;
            cep->validating = false;
            memcpy(&cep->ep, ep, sizeof(*ep));
            return cep;
        }
    }
    return NULL;
}

static void _free_client_ep(client_ep_t *cep)
{
    memset(cep, 0, sizeof(*cep));
}

static int _request_matcher_forward_proxy(gcoap_listener_t *listener,
                                          const coap_resource_t **resource,
                                          coap_pkt_t *pdu)
{
    (void) listener;

    char *offset;

    if (coap_get_proxy_uri(pdu, &offset) > 0) {
        *resource = &listener->resources[0];
        return GCOAP_RESOURCE_FOUND;
    }

    return GCOAP_RESOURCE_NO_PATH;
}

static ssize_t _forward_proxy_handler(coap_pkt_t *pdu, uint8_t *buf,
                                      size_t len, void *ctx)
{
    int pdu_len = 0;
    sock_udp_ep_t *remote = (sock_udp_ep_t *)ctx;

    pdu_len = gcoap_forward_proxy_request_process(pdu, remote);

    /* Out of memory, reply with 5.00 */
    if (pdu_len == -ENOMEM) {
        return gcoap_response(pdu, buf, len, COAP_CODE_INTERNAL_SERVER_ERROR);
    }
    /* Proxy-Uri malformed, reply with 4.02 */
    else if (pdu_len == -EINVAL) {
        return gcoap_response(pdu, buf, len, COAP_CODE_BAD_OPTION);
    }
    /* scheme not supported */
    else if (pdu_len == -EPERM) {
        return gcoap_response(pdu, buf, len, COAP_CODE_PROXYING_NOT_SUPPORTED);
    }

    return pdu_len;
}

static bool _parse_endpoint(sock_udp_ep_t *remote,
                            uri_parser_result_t *urip)
{
    char scratch[8];
    ipv6_addr_t addr;
    remote->family = AF_INET6;

    /* support IPv6 only for now */
    if (!urip->ipv6addr) {
        return false;
    }

    /* check for interface */
    if (urip->zoneid) {
        /* only works with integer based zoneids */

        if (urip->zoneid_len > (ARRAY_SIZE(scratch) - 1)) {
            return false;
        }

        memcpy(scratch, urip->zoneid, urip->zoneid_len);

        scratch[urip->zoneid_len] = '\0';

        int pid = atoi(scratch);

        if (gnrc_netif_get_by_pid(pid) == NULL) {
            return false;
        }
        remote->netif = pid;
    }
    /* no interface present */
    else {
        if (gnrc_netif_numof() == 1) {
            /* assign the single interface found in gnrc_netif_numof() */
            remote->netif = (uint16_t)gnrc_netif_iter(NULL)->pid;
        }
        else {
            remote->netif = SOCK_ADDR_ANY_NETIF;
        }
    }

    /* parse destination address */
    if (ipv6_addr_from_buf(&addr, urip->ipv6addr, urip->ipv6addr_len) == NULL) {
        return false;
    }
    if ((remote->netif == SOCK_ADDR_ANY_NETIF) &&
        ipv6_addr_is_link_local(&addr)) {
        return false;
    }
    memcpy(&remote->addr.ipv6[0], &addr.u8[0], sizeof(addr.u8));

    if (urip->port_len) {
        /* copy port string into scratch for atoi */
        memcpy(scratch, urip->port, urip->port_len);
        scratch[urip->port_len] = '\0';

        remote->port = atoi(scratch);

        if (remote->port == 0) {
          return false;
        }
    }
    else {
        remote->port = COAP_PORT;
    }

    return true;
}

static void _forward_resp_handler(const gcoap_request_memo_t *memo,
                                  coap_pkt_t* pdu,
                                  const sock_udp_ep_t *remote)
{
    (void) remote; /* this is the origin server */
    client_ep_t *cep = (client_ep_t *)memo->context;

    if (memo->state == GCOAP_MEMO_RESP) {
        if (!IS_USED(MODULE_NANOCOAP_CACHE) ||
            /* only forward 2.03 Valid, if client endpoint sent an ETag to validate cached content */
            (pdu->hdr->code != COAP_CODE_VALID) || cep->validating) {
            /* forward the response packet as-is to the client */
            gcoap_forward_proxy_dispatch((uint8_t *)pdu->hdr,
                                         (pdu->payload -
                                          (uint8_t *)pdu->hdr + pdu->payload_len),
                                         &cep->ep);
        }

#if IS_USED(MODULE_NANOCOAP_CACHE)
        /* if response is a 2.03, but client endpoint did not send an ETag to validate content */
        if ((pdu->hdr->code == COAP_CODE_VALID) && !cep->validating) {
            nanocoap_cache_entry_t *ce = NULL;

            if ((ce = nanocoap_cache_key_lookup(cep->cache_key))) {
                /* update max_age from response and send cached response */
                uint32_t max_age = 60;

                coap_opt_get_uint(pdu, COAP_OPT_MAX_AGE, &max_age);
                ce->max_age = ztimer_now(ZTIMER_SEC) + max_age;
                /* copy all options and possible payload from the cached response
                 * to the new response */
                unsigned header_len_req = coap_get_total_hdr_len(pdu);
                unsigned header_len_cached = coap_get_total_hdr_len(&ce->response_pkt);
                uint8_t *buf = (uint8_t *)pdu->hdr;
                size_t len = pdu->payload_len + header_len_req;
                gcoap_resp_init(pdu, buf, len, ce->response_pkt.hdr->code);
                unsigned opt_payload_len = ce->response_len - header_len_cached;

                memcpy((buf + header_len_req),
                       (ce->response_buf + header_len_cached),
                       opt_payload_len);
                gcoap_forward_proxy_dispatch(buf, header_len_req + opt_payload_len, &cep->ep);
            }
            else {
                /* cache entry to be validated cached out while trying to validate */
                /* TODO: re-request real response (without ETag) */
            }
        }
        else {
            coap_pkt_t req;
            if (memo->send_limit == GCOAP_SEND_LIMIT_NON) {
                req.hdr = (coap_hdr_t *) &memo->msg.hdr_buf[0];
            }
            else {
                req.hdr = (coap_hdr_t *) memo->msg.data.pdu_buf;
            }

            size_t pdu_len = pdu->payload_len +
                (pdu->payload - (uint8_t *)pdu->hdr);
            nanocoap_cache_process(cep->cache_key, coap_get_code(&req), pdu, pdu_len);
        }
#endif
    }
    _free_client_ep(cep);
}

static int _gcoap_forward_proxy_add_uri_path(coap_pkt_t *pkt,
                                             uri_parser_result_t *urip)
{
    ssize_t res = coap_opt_add_chars(pkt, COAP_OPT_URI_PATH,
                                     urip->path, urip->path_len, '/');
    if (res < 0) {
        return -EINVAL;
    }

    if (urip->query) {
        res = coap_opt_add_chars(pkt, COAP_OPT_URI_QUERY,
                                 urip->query, urip->query_len, '&');
        if (res < 0) {
            return -EINVAL;
        }
    }

    return 0;
}

static int _gcoap_forward_proxy_copy_options(coap_pkt_t *pkt,
                                             coap_pkt_t *client_pkt,
                                             client_ep_t *cep,
                                             uri_parser_result_t *urip,
                                             nanocoap_cache_entry_t *ce)
{
    /* copy all options from client_pkt to pkt */
    coap_optpos_t opt = {0, 0};
    uint8_t *value;
    bool uri_path_added = false, etag_added = false;

    for (int i = 0; i < client_pkt->options_len; i++) {
        ssize_t optlen = coap_opt_get_next(client_pkt, &opt, &value, !i);
        /* wrt to ETag: we always have at least the Proxy-URI option in the client_pkt, so
         * we should hit at least once (and its opt_num is also >= COAP_OPT_ETAG) */
        if (optlen >= 0) {
            /* Add ETag before any larger opt num, but skip old ETag if ETag in cache */
            if (!etag_added && (opt.opt_num >= COAP_OPT_ETAG)) {
                if (IS_USED(MODULE_NANOCOAP_CACHE) && ce) {
                    uint8_t *etag;
                    /* Searching for more ETags might become necessary in the future */
                    ssize_t etag_len = coap_opt_get_opaque(&ce->response_pkt, COAP_OPT_ETAG, &etag);

                    if (etag_len > 0) {
                        coap_opt_add_opaque(pkt, COAP_OPT_ETAG, etag, etag_len);
                    }
                    etag_added = true;
                }
            }
            /* skip original ETag of request, otherwise we might accidentally fill the cache
             * with 2.03 Valid responses which would require additional handling */
            if (opt.opt_num == COAP_OPT_ETAG) {
                cep->validating = true;
                continue;
            }
            /* add URI-PATH before any larger opt num */
            if (!uri_path_added && (opt.opt_num > COAP_OPT_URI_PATH)) {
                if (_gcoap_forward_proxy_add_uri_path(pkt, urip) == -EINVAL) {
                    return -EINVAL;
                }
                uri_path_added = true;
            }
            /* skip PROXY-URI in new packet */
            if (opt.opt_num == COAP_OPT_PROXY_URI) {
                continue;
            }
            /* the actual copy operation */
            coap_opt_add_opaque(pkt, opt.opt_num, value, optlen);
        }
    }

    ssize_t len = coap_opt_finish(pkt,
                                  (client_pkt->payload_len ?
                                   COAP_OPT_FINISH_PAYLOAD :
                                   COAP_OPT_FINISH_NONE));

    /* copy payload from client_pkt to pkt */
    memcpy(pkt->payload, client_pkt->payload, client_pkt->payload_len);
    len += client_pkt->payload_len;

    return len;
}

static int _gcoap_forward_proxy_via_coap(coap_pkt_t *client_pkt,
                                         client_ep_t *client_ep,
                                         uri_parser_result_t *urip,
                                         nanocoap_cache_entry_t *ce)
{
    coap_pkt_t pkt;
    sock_udp_ep_t origin_server_ep;

    ssize_t len;
    gcoap_request_memo_t *memo = NULL;

    if (!_parse_endpoint(&origin_server_ep, urip)) {
        return -EINVAL;
    }

    /* do not forward requests if they already exist, e.g., due to CON
       and retransmissions. In the future, the proxy should set an
       empty ACK message to stop the retransmissions of a client */
    gcoap_forward_proxy_find_req_memo(&memo, client_pkt, &origin_server_ep);
    if (memo) {
        DEBUG("gcoap_forward_proxy: request already exists, ignore!\n");
        _free_client_ep(client_ep);
        return 0;
    }

    unsigned token_len = coap_get_token_len(client_pkt);

    coap_pkt_init(&pkt, proxy_req_buf, CONFIG_GCOAP_PDU_BUF_SIZE,
                  sizeof(coap_hdr_t) + token_len);

    pkt.hdr->ver_t_tkl = client_pkt->hdr->ver_t_tkl;
    pkt.hdr->code = client_pkt->hdr->code;
    pkt.hdr->id = client_pkt->hdr->id;

    if (token_len) {
        memcpy(coap_get_token(&pkt), coap_get_token(client_pkt), token_len);
    }

    /* copy all options from client_pkt to pkt */
    len = _gcoap_forward_proxy_copy_options(&pkt, client_pkt, client_ep, urip, ce);

    if (len == -EINVAL) {
        return -EINVAL;
    }

    len = gcoap_req_send((uint8_t *)pkt.hdr, len,
                         &origin_server_ep,
                         _forward_resp_handler, (void *)client_ep);
    return len;
}

int gcoap_forward_proxy_request_process(coap_pkt_t *pkt,
                                        sock_udp_ep_t *client) {
    char *uri;
    uri_parser_result_t urip;
    ssize_t optlen = 0;

    client_ep_t *cep = _allocate_client_ep(client);
    nanocoap_cache_entry_t *ce = NULL;

    if (!cep) {
        return -ENOMEM;
    }

    if (IS_USED(MODULE_NANOCOAP_CACHE)) {
        int pdu_len = _cache_lookup_and_process(pkt,
                                                (uint8_t *)pkt->hdr,
                                                CONFIG_GCOAP_PDU_BUF_SIZE,
                                                cep,
                                                &ce);
        /* if a valid cache entry was found, then pdu_len contains the
         * length of that response message */
        if (pdu_len > 0) {
            _free_client_ep(cep);
            return pdu_len;
        }
        /* if there was no cache hit, then we continue forwarding */
    }

    optlen = coap_get_proxy_uri(pkt, &uri);

    if (optlen < 0) {
        /* -ENOENT, -EINVAL */
        _free_client_ep(cep);
        return optlen;
    }

    int ures = uri_parser_process(&urip, (const char *) uri, optlen);

    /* cannot parse Proxy-URI option, or URI is relative */
    if (ures || (!uri_parser_is_absolute((const char *) uri, optlen))) {
        _free_client_ep(cep);
        return -EINVAL;
    }

    /* target is using CoAP */
    if (!strncmp("coap", urip.scheme, urip.scheme_len)) {
        int res = _gcoap_forward_proxy_via_coap(pkt, cep, &urip, ce);
        if (res < 0) {
            _free_client_ep(cep);
            return -EINVAL;
        }
    }
    /* no other scheme supported for now */
    else {
        _free_client_ep(cep);
        return -EPERM;
    }

    return 0;
}

/** @} */