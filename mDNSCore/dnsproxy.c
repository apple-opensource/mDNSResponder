/* -*- Mode: C; tab-width: 4 -*-
 *
 * Copyright (c) 2011-2020 Apple Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "dnsproxy.h"

#if MDNSRESPONDER_SUPPORTS(APPLE, DNS_PROXY_DNS64)
#include <nw/private.h>
#endif

#ifndef UNICAST_DISABLED

extern mDNS mDNSStorage;
#if MDNSRESPONDER_SUPPORTS(APPLE, DNS_PROXY_DNS64)
static mDNSBool gDNS64Enabled = mDNSfalse;
static mDNSBool gDNS64ForceAAAASynthesis = mDNSfalse;
static nw_nat64_prefix_t gDNS64Prefix;
#endif

// Implementation Notes
//
// DNS Proxy listens on port 53 (UDPv4v6 & TCPv4v6) for DNS queries. It handles only
// the "Query" opcode of the DNS protocol described in RFC 1035. For all other opcodes, it returns
// "Not Implemented" error. The platform interface mDNSPlatformInitDNSProxySkts
// sets up the sockets and whenever it receives a packet, it calls ProxyTCPCallback or ProxyUDPCallback
// defined here. For TCP socket, the platform does the "accept" and only sends the received packets
// on the newly accepted socket. A single UDP socket (per address family) is used to send/recv
// requests/responses from all clients. For TCP, there is one socket per request. Hence, there is some
// extra state that needs to be disposed at the end.
//
// When a DNS request is received, ProxyCallbackCommon checks for malformed packet etc. and also checks
// for duplicates, before creating DNSProxyClient state and starting a question with the "core"
// (mDNS_StartQuery). When the callback for the question happens, it gathers all the necessary
// resource records, constructs a response and sends it back to the client.
//
//   - Question callback is called with only one resource record at a time. We need all the resource
//     records to construct the response. Hence, we lookup all the records ourselves. 
//
//   - The response may not fit the client's buffer size. In that case, we need to set the truncate bit
//     and the client would retry using TCP.
//
//   - The client may have set the DNSSEC OK bit in the EDNS0 option and that means we also have to
//     return the RRSIGs or the NSEC records with the RRSIGs in the Additional section. We need to
//     ask the "core" to fetch the DNSSEC records and do the validation if the CD bit is not set.
//
// Once the response is sent to the client, the client state is disposed. When there is no response
// from the "core", it eventually times out and we will not find any answers in the cache and we send a
// "NXDomain" response back. Thus, we don't need any special timers to reap the client state in the case
// of errors. 

typedef struct DNSProxyClient_struct DNSProxyClient;

#if MDNSRESPONDER_SUPPORTS(APPLE, DNS_PROXY_DNS64)
typedef enum
{
    kDNSProxyDNS64State_Initial                 = 0,    // Initial state.
    kDNSProxyDNS64State_AAAASynthesis           = 1,    // Querying for A record for AAAA record synthesis.
    kDNSProxyDNS64State_PTRSynthesisTrying      = 2,    // Querying for in-addr.arpa PTR record to map from ip6.arpa PTR.
    kDNSProxyDNS64State_PTRSynthesisSuccess     = 3,    // in-addr.arpa PTR query got non-negative non-CNAME answer.
    kDNSProxyDNS64State_PTRSynthesisNXDomain    = 4     // in-addr.arpa PTR query produced no useful result.

}   DNSProxyDNS64State;
#endif

struct DNSProxyClient_struct {

    DNSProxyClient *next; 
    mDNSAddr    addr;               // Client's IP address 
    mDNSIPPort  port;               // Client's port number
    mDNSOpaque16 msgid;             // DNS msg id
    mDNSInterfaceID interfaceID;    // Interface on which we received the request
    void *socket;                   // Return socket
    mDNSBool tcp;                   // TCP or UDP ?
    mDNSOpaque16 requestFlags;      // second 16 bit word in the DNSMessageHeader of the request
    mDNSu8 *optRR;                  // EDNS0 option
    mDNSu16 optLen;                 // Total Length of the EDNS0 option 
    mDNSu16 rcvBufSize;             // How much can the client receive ?
    void *context;                  // Platform context to be disposed if non-NULL
    domainname qname;               // q->qname can't be used for duplicate check
    DNSQuestion q;                  // as it can change underneath us for CNAMEs
    mDNSu16 qtype;
#if MDNSRESPONDER_SUPPORTS(APPLE, DNS_PROXY_DNS64)
    DNSProxyDNS64State dns64state;
#endif
};

#define MIN_DNS_MESSAGE_SIZE    512
static DNSProxyClient *DNSProxyClients;

mDNSlocal void FreeDNSProxyClient(DNSProxyClient *pc)
{
    if (pc->optRR)
        mDNSPlatformMemFree(pc->optRR);
    mDNSPlatformMemFree(pc);
}

mDNSlocal mDNSBool ParseEDNS0(DNSProxyClient *pc, const mDNSu8 *ptr, int length, const mDNSu8 *limit)
{
    if (ptr + length > limit)
    {
        LogInfo("ParseEDNS0: Not enough space in the packet");
        return mDNSfalse;
    }
    // Skip the root label
    ptr++;
    mDNSu16 rrtype  = (mDNSu16) ((mDNSu16)ptr[0] <<  8 | ptr[1]);
    if (rrtype != kDNSType_OPT)
    {
        LogInfo("ParseEDNS0: Not the right type %d", rrtype);
        return mDNSfalse;
    }
    mDNSu16 rrclass = (mDNSu16) ((mDNSu16)ptr[2] <<  8 | ptr[3]);
#if MDNS_DEBUGMSGS
    mDNSu8  rcode   = ptr[4];
    mDNSu8  version = ptr[5];
    mDNSu16 flag    = (mDNSu16) ((mDNSu16)ptr[6] <<  8 | ptr[7]);
    debugf("rrtype is %s, length is %d, rcode %d, version %d, flag 0x%x", DNSTypeName(rrtype), rrclass, rcode, version, flag);
#endif
    pc->rcvBufSize = rrclass;
    
    return mDNStrue;
}

mDNSexport mDNSu8 *DNSProxySetAttributes(DNSQuestion *q, DNSMessageHeader *h, DNSMessage *msg, mDNSu8 *ptr, mDNSu8 *limit)
{
    DNSProxyClient *pc = (DNSProxyClient *)q->QuestionContext;

    (void) msg;

    h->flags = pc->requestFlags;
    if (pc->optRR)
    {
        if (ptr + pc->optLen > limit)
        {
            LogInfo("DNSProxySetAttributes: Cannot set EDNS0 option start %p, OptLen %d, end %p", ptr, pc->optLen, limit);
            return ptr;
        }
        h->numAdditionals++;
        mDNSPlatformMemCopy(ptr, pc->optRR, pc->optLen);
        ptr += pc->optLen;
    }
    return ptr;
}

mDNSlocal mDNSu8 *AddEDNS0Option(mDNSu8 *ptr, mDNSu8 *limit)
{
    int len = 4096;

    if (ptr + 11 > limit)
    {
        LogInfo("AddEDNS0Option: not enough space");
        return mDNSNULL;
    }
    mDNSStorage.omsg.h.numAdditionals++;
    ptr[0] = 0;
    ptr[1] = (mDNSu8) (kDNSType_OPT >> 8);
    ptr[2] = (mDNSu8) (kDNSType_OPT & 0xFF);
    ptr[3] = (mDNSu8) (len >> 8);
    ptr[4] = (mDNSu8) (len & 0xFF);
    ptr[5] = 0;     // rcode
    ptr[6] = 0;     // version
    ptr[7] = 0;
    ptr[8] = 0;     // flags
    ptr[9] = 0;     // rdlength
    ptr[10] = 0;    // rdlength

    debugf("AddEDNS0 option");

    return (ptr + 11);
}

// Currently RD and CD bit should be copied if present in the request or cleared if
// not present in the request. RD bit is normally set in the response and hence the
// cache reflects the right value. CD bit behaves differently. If the CD bit is set
// the first time, the cache retains it, if it is present in response (assuming the
// upstream server does it right). Next time through we should not use the cached
// value of the CD bit blindly. It depends on whether it was in the request or not.
mDNSlocal mDNSOpaque16 SetResponseFlags(DNSProxyClient *pc, const mDNSOpaque16 responseFlags)
{
    mDNSOpaque16 rFlags = responseFlags;

    if (pc->requestFlags.b[0] & kDNSFlag0_RD)
        rFlags.b[0] |= kDNSFlag0_RD;
    else
        rFlags.b[0] &= ~kDNSFlag0_RD;

    if (pc->requestFlags.b[1] & kDNSFlag1_CD)
        rFlags.b[1] |= kDNSFlag1_CD;
    else
        rFlags.b[1] &= ~kDNSFlag1_CD;

    return rFlags;
}

mDNSlocal mDNSu8 *AddResourceRecords(DNSProxyClient *pc, mDNSu8 **prevptr, mStatus *error)
{
    mDNS *const m = &mDNSStorage;
    CacheGroup *cg;
    CacheRecord *cr;
    int len = sizeof(DNSMessageHeader);
    mDNSu8 *orig = m->omsg.data;
    mDNSBool first = mDNStrue;
    mDNSu8 *ptr = mDNSNULL;
    mDNSs32 now;
    mDNSs32 ttl;
    const CacheRecord *soa = mDNSNULL;
    const CacheRecord *cname = mDNSNULL;
    mDNSu8 *limit;
    domainname tempQName;
    mDNSu32 tempQNameHash;

    *error = mStatus_NoError;
    *prevptr = mDNSNULL;

    mDNS_Lock(m);
    now = m->timenow;
    mDNS_Unlock(m);

    if (!pc->tcp)
    {
        if (!pc->rcvBufSize)
        {
            limit = m->omsg.data + MIN_DNS_MESSAGE_SIZE;
        }
        else
        {
            limit = (pc->rcvBufSize > AbsoluteMaxDNSMessageData ? m->omsg.data + AbsoluteMaxDNSMessageData : m->omsg.data + pc->rcvBufSize);
        }
    }
    else
    {
        // For TCP, limit is not determined by EDNS0 but by 16 bit rdlength field and
        // AbsoluteMaxDNSMessageData is smaller than 64k.
        limit = m->omsg.data + AbsoluteMaxDNSMessageData;
    }
    LogInfo("AddResourceRecords: Limit is %d", limit - m->omsg.data);

#if MDNSRESPONDER_SUPPORTS(APPLE, DNS_PROXY_DNS64)
    if (pc->dns64state == kDNSProxyDNS64State_PTRSynthesisSuccess)
    {
        // We're going to synthesize a CNAME record to map the originally requested ip6.arpa domain name to the
        // in-addr.arpa domain name, so we use pc->q.qname, which contains the in-addr.arpa domain name, to get the
        // in-addr.arpa PTR record.
        AssignDomainName(&tempQName, &pc->q.qname);
        tempQNameHash = DomainNameHashValue(&tempQName);
    }
    else
#endif
    {
        AssignDomainName(&tempQName, &pc->qname);
        tempQNameHash = DomainNameHashValue(&tempQName);
    }

again:
    soa = cname = mDNSNULL;

    cg = CacheGroupForName(m, tempQNameHash, &tempQName);
    if (!cg)
    {
        LogInfo("AddResourceRecords: CacheGroup not found for %##s", tempQName.c);
        *error = mStatus_NoSuchRecord;
        return mDNSNULL;
    }
    for (cr = cg->members; cr; cr = cr->next)
    {
        if (SameNameCacheRecordAnswersQuestion(cr, &pc->q))
        {
            if (first)
            {
                // If this is the first time, initialize the header and the question.
                // This code needs to be here so that we can use the responseFlags from the
                // cache record
                mDNSOpaque16 responseFlags = SetResponseFlags(pc, cr->responseFlags);
                InitializeDNSMessage(&m->omsg.h, pc->msgid, responseFlags);
                ptr = putQuestion(&m->omsg, m->omsg.data, m->omsg.data + AbsoluteMaxDNSMessageData, &pc->qname, pc->qtype, pc->q.qclass);
                if (!ptr)
                {
                    LogInfo("AddResourceRecords: putQuestion NULL for %##s (%s)", &pc->qname.c, DNSTypeName(pc->qtype));
                    return mDNSNULL;
                }
                first = mDNSfalse;
#if MDNSRESPONDER_SUPPORTS(APPLE, DNS_PROXY_DNS64)
                if (pc->dns64state == kDNSProxyDNS64State_PTRSynthesisSuccess)
                {
                    // For the first answer record, synthesize a CNAME record to map the originally requested ip6.arpa
                    // domain name to the in-addr.arpa domain name.
                    // See <https://tools.ietf.org/html/rfc6147#section-5.3.1>.
                    RData rdata;
                    ResourceRecord newRR;
                    mDNSPlatformMemZero(&newRR, (mDNSu32)sizeof(newRR));
                    newRR.RecordType    = kDNSRecordTypePacketAns;
                    newRR.rrtype        = kDNSType_CNAME;
                    newRR.rrclass       = kDNSClass_IN;
                    newRR.name          = &pc->qname;
                    AssignDomainName(&rdata.u.name, &pc->q.qname);
                    rdata.MaxRDLength   = (mDNSu32)sizeof(rdata.u);
                    newRR.rdata         = &rdata;
                    ptr = PutResourceRecordTTLWithLimit(&m->omsg, ptr, &m->omsg.h.numAnswers, &newRR, 0, limit);
                    if (!ptr)
                    {
                        *prevptr = orig;
                        return mDNSNULL;
                    }
                }
#endif
            }
            // - For NegativeAnswers there is nothing to add
            // - If DNSSECOK is set, we also automatically lookup the RRSIGs which
            //   will also be returned. If the client is explicitly looking up
            //   a DNSSEC record (e.g., DNSKEY, DS) we should return the response.
            //   DNSSECOK bit only influences whether we add the RRSIG or not.
            if (cr->resrec.RecordType != kDNSRecordTypePacketNegative)
            {
                const ResourceRecord *rr;
#if MDNSRESPONDER_SUPPORTS(APPLE, DNS_PROXY_DNS64)
                RData rdata;
                ResourceRecord newRR;
                if ((pc->dns64state == kDNSProxyDNS64State_AAAASynthesis) && (cr->resrec.rrtype == kDNSType_A))
                {
                    struct in_addr  addrV4;
                    struct in6_addr addrV6;

                    newRR               = cr->resrec;
                    newRR.rrtype        = kDNSType_AAAA;
                    newRR.rdlength      = 16;
                    rdata.MaxRDLength   = newRR.rdlength;
                    newRR.rdata         = &rdata;

                    memcpy(&addrV4.s_addr, cr->resrec.rdata->u.ipv4.b, 4);
                    if (nw_nat64_synthesize_v6(&gDNS64Prefix, &addrV4, &addrV6))
                    {
                        memcpy(rdata.u.ipv6.b, addrV6.s6_addr, 16);
                        rr = &newRR;
                    }
                    else
                    {
                        continue;
                    }
                }
                else
#endif
                {
                    rr = &cr->resrec;
                }
                LogInfo("AddResourceRecords: Answering question with %s", RRDisplayString(m, rr));
                ttl = cr->resrec.rroriginalttl - (now - cr->TimeRcvd) / mDNSPlatformOneSecond;
                ptr = PutResourceRecordTTLWithLimit(&m->omsg, ptr, &m->omsg.h.numAnswers, rr, ttl, limit);
                if (!ptr)
                {
                    *prevptr = orig;
                    return mDNSNULL;
                }
                len += (ptr - orig); 
                orig = ptr;
            }
            if (cr->soa)
            {
                LogInfo("AddResourceRecords: soa set for %s", CRDisplayString(m ,cr));
                soa = cr->soa;
            }
            // If we are using CNAME to answer a question and CNAME is not the type we
            // are looking for, note down the CNAME record so that we can follow them
            // later. Before we follow the CNAME, print the RRSIGs and any nsec (wildcard
            // expanded) if any.
            if ((pc->q.qtype != cr->resrec.rrtype) && cr->resrec.rrtype == kDNSType_CNAME)
            {
                LogInfo("AddResourceRecords: cname set for %s", CRDisplayString(m ,cr));
                cname = cr;
            }
        }
    }
    // Along with the nsec records, we also cache the SOA record. For non-DNSSEC question, we need
    // to send the SOA back. Normally we either cache the SOA record (non-DNSSEC question) pointed
    // to by "cr->soa" or the NSEC/SOA records along with their RRSIGs (DNSSEC question) pointed to
    // by "cr->nsec". Two cases:
    //
    // - if we issue a DNSSEC question followed by non-DNSSEC question for the same name,
    //   we only have the nsec records and we need to filter the SOA record alone for the
    //   non-DNSSEC questions.
    //
    // - if we issue a non-DNSSEC question followed by DNSSEC question for the same name,
    //   the "core" flushes the cache entry and re-issue the question with EDNS0/DOK bit and
    //   in this case we return all the DNSSEC records we have.
    if (soa)
    {
        LogInfo("AddResourceRecords: SOA Answering question with %s", CRDisplayString(m, soa));
        ptr = PutResourceRecordTTLWithLimit(&m->omsg, ptr, &m->omsg.h.numAuthorities, &soa->resrec, soa->resrec.rroriginalttl, limit);
        if (!ptr)
        {
            *prevptr = orig;
            return mDNSNULL;
        }
        len += (ptr - orig); 
        orig = ptr;
    }
    if (cname)
    {
        AssignDomainName(&tempQName, &cname->resrec.rdata->u.name);
        tempQNameHash = DomainNameHashValue(&tempQName);
        goto again;
    }
    if (!ptr)
    {
        LogInfo("AddResourceRecords: Did not find any valid ResourceRecords");
        *error = mStatus_NoSuchRecord;
        return mDNSNULL;
    }
    if (pc->rcvBufSize)
    {
        ptr = AddEDNS0Option(ptr, limit);
        if (!ptr)
        {
            *prevptr = orig;
            return mDNSNULL;
        }
        len += (ptr - orig); 
        // orig = ptr; Commented out to avoid ‘value never read’ error message
    }
    LogInfo("AddResourceRecord: Added %d bytes to the packet", len);
    return ptr;
}

mDNSlocal void ProxyClientCallback(mDNS *const m, DNSQuestion *question, const ResourceRecord *const answer, QC_result AddRecord)
{
    DNSProxyClient *pc = question->QuestionContext;
    DNSProxyClient **ppc = &DNSProxyClients;
    mDNSu8 *ptr;
    mDNSu8 *prevptr;
    mStatus error;

    if (!AddRecord)
        return;

    LogInfo("ProxyClientCallback: ResourceRecord %s", RRDisplayString(m, answer));

#if MDNSRESPONDER_SUPPORTS(APPLE, DNS_PROXY_DNS64)
    if (gDNS64Enabled)
    {
        if (pc->dns64state == kDNSProxyDNS64State_Initial)
        {
            // If we get a negative AAAA answer, then retry the query as an A record query.
            // See <https://tools.ietf.org/html/rfc6147#section-5.1.6>.
            if ((answer->RecordType == kDNSRecordTypePacketNegative) && (question->qtype == kDNSType_AAAA) &&
                (answer->rrtype == kDNSType_AAAA) && (answer->rrclass == kDNSClass_IN))
            {
                mDNS_StopQuery(m, question);
                pc->dns64state = kDNSProxyDNS64State_AAAASynthesis;
                question->qtype = kDNSType_A;
                mDNS_StartQuery(m, question);
                return;
            }
        }
        else if (pc->dns64state == kDNSProxyDNS64State_PTRSynthesisTrying)
        {
            // If we get a non-negative non-CNAME answer, then this is the answer we give to the client.
            // Otherwise, just respond with NXDOMAIN.
            // See <https://tools.ietf.org/html/rfc6147#section-5.3.1>.
            if ((answer->RecordType != kDNSRecordTypePacketNegative) && (question->qtype == kDNSType_PTR) &&
                (answer->rrtype == kDNSType_PTR) && (answer->rrclass == kDNSClass_IN))
            {
                pc->dns64state = kDNSProxyDNS64State_PTRSynthesisSuccess;
            }
            else
            {
                pc->dns64state = kDNSProxyDNS64State_PTRSynthesisNXDomain;
            }
        }
    }
    if (pc->dns64state == kDNSProxyDNS64State_PTRSynthesisNXDomain)
    {
        const mDNSOpaque16 flags = { { kDNSFlag0_QR_Response | kDNSFlag0_OP_StdQuery, kDNSFlag1_RC_NXDomain } };
        InitializeDNSMessage(&m->omsg.h, pc->msgid, flags);
        ptr = putQuestion(&m->omsg, m->omsg.data, m->omsg.data + AbsoluteMaxDNSMessageData, &pc->qname, pc->qtype,
            pc->q.qclass);
        if (!ptr)
        {
            LogInfo("ProxyClientCallback: putQuestion NULL for %##s (%s)", &pc->qname.c, DNSTypeName(pc->qtype));
        }
    }
    else
#endif
    {
        if ((answer->RecordType != kDNSRecordTypePacketNegative) && (answer->rrtype != question->qtype))
        {
            // Wait till we get called for the real response
            LogInfo("ProxyClientCallback: Received %s, not answering yet", RRDisplayString(m, answer));
            return;
        }
        ptr = AddResourceRecords(pc, &prevptr, &error);
        if (!ptr)
        {
            LogInfo("ProxyClientCallback: AddResourceRecords NULL for %##s (%s)", &pc->qname.c, DNSTypeName(pc->qtype));
            if (error == mStatus_NoError && prevptr)
            {
                // No space to add the record. Set the Truncate bit for UDP.
                //
                // TBD: For TCP, we need to send the rest of the data. But finding out what is left
                // is harder. We should allocate enough buffer in the first place to send all
                // of the data.
                if (!pc->tcp)
                {
                    m->omsg.h.flags.b[0] |= kDNSFlag0_TC;
                    ptr = prevptr;
                }
                else
                {
                    LogInfo("ProxyClientCallback: ERROR!! Not enough space to return in TCP for %##s (%s)", &pc->qname.c, DNSTypeName(pc->qtype));
                    ptr = prevptr;
                }
            }
            else
            {
                mDNSOpaque16 flags   = { { kDNSFlag0_QR_Response | kDNSFlag0_OP_StdQuery, kDNSFlag1_RC_ServFail } };
                // We could not find the record for some reason. Return a response, so that the client
                // is not waiting forever.
                LogInfo("ProxyClientCallback: No response");
                if (!mDNSOpaque16IsZero(pc->q.responseFlags))
                    flags = pc->q.responseFlags;
                InitializeDNSMessage(&m->omsg.h, pc->msgid, flags);
                ptr = putQuestion(&m->omsg, m->omsg.data, m->omsg.data + AbsoluteMaxDNSMessageData, &pc->qname, pc->qtype, pc->q.qclass);
                if (!ptr)
                {
                    LogInfo("ProxyClientCallback: putQuestion NULL for %##s (%s)", &pc->qname.c, DNSTypeName(pc->qtype));
                    goto done;
                }
            }
        }
    }
    debugf("ProxyClientCallback: InterfaceID is %p for response to client", pc->interfaceID);

    if (!pc->tcp)
    {
        mDNSSendDNSMessage(m, &m->omsg, ptr, pc->interfaceID, mDNSNULL, (UDPSocket *)pc->socket, &pc->addr, pc->port, mDNSNULL, mDNSfalse);
    }
    else
    {
        mDNSSendDNSMessage(m, &m->omsg, ptr, pc->interfaceID, (TCPSocket *)pc->socket, mDNSNULL, &pc->addr, pc->port, mDNSNULL, mDNSfalse);
    }

done:
    mDNS_StopQuery(m, question);

    while (*ppc && *ppc != pc)
        ppc=&(*ppc)->next;
    if (!*ppc)
    {
        LogMsg("ProxyClientCallback: question %##s (%s) not found", question->qname.c, DNSTypeName(question->qtype));
        return;
    }
    *ppc = pc->next;
    mDNSPlatformDisposeProxyContext(pc->context);
    FreeDNSProxyClient(pc);
}

mDNSlocal void SendError(void *socket, DNSMessage *const msg, const mDNSu8 *const end, const mDNSAddr *dstaddr,
    const mDNSIPPort dstport, const mDNSInterfaceID InterfaceID, mDNSBool tcp, void *context, mDNSu8 rcode)
{
    mDNS *const m = &mDNSStorage;
    int pktlen = (int)(end - (mDNSu8 *)msg);

    // RFC 1035 requires that we copy the question back and RFC 2136 is okay with sending nothing
    // in the body or send back whatever we get for updates. It is easy to return whatever we get
    // in the question back to the responder. We return as much as we can fit in our standard
    // output packet.
    if (pktlen > AbsoluteMaxDNSMessageData)
        pktlen = AbsoluteMaxDNSMessageData;

    mDNSPlatformMemCopy(&m->omsg.h, &msg->h, sizeof(DNSMessageHeader));
    m->omsg.h.flags.b[0] |= kDNSFlag0_QR_Response;
    m->omsg.h.flags.b[1] = rcode;
    mDNSPlatformMemCopy(m->omsg.data, (mDNSu8 *)&msg->data, (pktlen - sizeof(DNSMessageHeader)));
    
    if (!tcp)
    {
        mDNSSendDNSMessage(m, &m->omsg, (mDNSu8 *)&m->omsg + pktlen, InterfaceID, mDNSNULL, socket, dstaddr, dstport, mDNSNULL, mDNSfalse);
    }
    else
    {
        mDNSSendDNSMessage(m, &m->omsg, (mDNSu8 *)&m->omsg + pktlen, InterfaceID, (TCPSocket *)socket, mDNSNULL, dstaddr, dstport, mDNSNULL, mDNSfalse);
    }
    mDNSPlatformDisposeProxyContext(context);
}

mDNSlocal DNSQuestion *IsDuplicateClient(const mDNSAddr *const addr, const mDNSIPPort port, const mDNSOpaque16 id,
    const DNSQuestion *const question)
{
    DNSProxyClient *pc;

    for (pc = DNSProxyClients; pc; pc = pc->next)
    {
        if (mDNSSameAddress(&pc->addr, addr)   &&
            mDNSSameIPPort(pc->port, port)  &&
            mDNSSameOpaque16(pc->msgid, id) &&
            pc->qtype == question->qtype  &&
            pc->q.qclass  == question->qclass &&
            SameDomainName(&pc->qname, &question->qname))
        {
            LogInfo("IsDuplicateClient: Found a duplicate client in the list");
            return(&pc->q);
        }
    }
    return(mDNSNULL);
}

mDNSlocal mDNSBool CheckDNSProxyIpIntf(mDNSInterfaceID InterfaceID)
{
    mDNS *const m = &mDNSStorage;
    int i;
    mDNSu32 ip_ifindex = (mDNSu32)(unsigned long)InterfaceID;

    LogInfo("CheckDNSProxyIpIntf: Check for ifindex[%d] in stored input interface list: [%d] [%d] [%d] [%d] [%d]",
            ip_ifindex, m->dp_ipintf[0], m->dp_ipintf[1], m->dp_ipintf[2], m->dp_ipintf[3], m->dp_ipintf[4]);

    if (ip_ifindex > 0)
    {
        for (i = 0; i < MaxIp; i++)
        {
            if (ip_ifindex == m->dp_ipintf[i])
                return mDNStrue;
        }
    }
    
    LogMsg("CheckDNSProxyIpIntf: ifindex[%d] not in stored input interface list: [%d] [%d] [%d] [%d] [%d]",
            ip_ifindex, m->dp_ipintf[0], m->dp_ipintf[1], m->dp_ipintf[2], m->dp_ipintf[3], m->dp_ipintf[4]);
    
    return mDNSfalse;

}

mDNSlocal void ProxyCallbackCommon(void *socket, DNSMessage *const msg, const mDNSu8 *const end, const mDNSAddr *const srcaddr,
    const mDNSIPPort srcport, const mDNSAddr *dstaddr, const mDNSIPPort dstport, const mDNSInterfaceID InterfaceID, mDNSBool tcp, void *context)
{
    mDNS *const m = &mDNSStorage;
    mDNSu8 QR_OP;
    const mDNSu8 *ptr;
    DNSQuestion q, *qptr;
    DNSProxyClient *pc;
    const mDNSu8 *optRR = mDNSNULL;
    int optLen = 0;
    DNSProxyClient **ppc = &DNSProxyClients;

    (void) dstaddr;
    (void) dstport;

    debugf("ProxyCallbackCommon: DNS Query coming from InterfaceID %p", InterfaceID);
    // Ignore if the DNS Query is not from a Valid Input InterfaceID
    if (!CheckDNSProxyIpIntf(InterfaceID))
    {
        LogMsg("ProxyCallbackCommon: Rejecting DNS Query coming from InterfaceID %p", InterfaceID);
        return;
    }
    
    if ((unsigned)(end - (mDNSu8 *)msg) < sizeof(DNSMessageHeader))
    {
        debugf("ProxyCallbackCommon: DNS Message from %#a:%d to %#a:%d length %d too short", srcaddr, mDNSVal16(srcport), dstaddr, mDNSVal16(dstport), (int)(end - (mDNSu8 *)msg));
        return;
    }

    // Read the integer parts which are in IETF byte-order (MSB first, LSB second)
    ptr = (mDNSu8 *)&msg->h.numQuestions;
    msg->h.numQuestions   = (mDNSu16)((mDNSu16)ptr[0] << 8 | ptr[1]);
    msg->h.numAnswers     = (mDNSu16)((mDNSu16)ptr[2] << 8 | ptr[3]);
    msg->h.numAuthorities = (mDNSu16)((mDNSu16)ptr[4] << 8 | ptr[5]);
    msg->h.numAdditionals = (mDNSu16)((mDNSu16)ptr[6] << 8 | ptr[7]);

    QR_OP = (mDNSu8)(msg->h.flags.b[0] & kDNSFlag0_QROP_Mask);
    if (QR_OP != kDNSFlag0_QR_Query)
    {
        LogInfo("ProxyCallbackCommon: Not a query(%d) for pkt from %#a:%d", QR_OP, srcaddr, mDNSVal16(srcport));
        SendError(socket, msg, end, srcaddr, srcport, InterfaceID, tcp, context, kDNSFlag1_RC_NotImpl);
        return;
    }
    
    if (msg->h.numQuestions != 1 || msg->h.numAnswers || msg->h.numAuthorities)
    {
        LogInfo("ProxyCallbackCommon: Malformed pkt from %#a:%d, Q:%d, An:%d, Au:%d", srcaddr, mDNSVal16(srcport),
            msg->h.numQuestions, msg->h.numAnswers, msg->h.numAuthorities);
        SendError(socket, msg, end, srcaddr, srcport, InterfaceID, tcp, context, kDNSFlag1_RC_FormErr);
        return;
    }
    ptr = msg->data;
    ptr = getQuestion(msg, ptr, end, InterfaceID, &q);
    if (!ptr)
    {
        LogInfo("ProxyCallbackCommon: Question cannot be parsed for pkt from %#a:%d", srcaddr, mDNSVal16(srcport));
        SendError(socket, msg, end, srcaddr, srcport, InterfaceID, tcp, context, kDNSFlag1_RC_FormErr);
        return;
    }
    else
    {
        LogInfo("ProxyCallbackCommon: Question %##s (%s)", q.qname.c, DNSTypeName(q.qtype));
    }
    ptr = LocateOptRR(msg, end, 0);
    if (ptr)
    {
        optRR = ptr;
        ptr = skipResourceRecord(msg, ptr, end);
        // Be liberal and ignore the EDNS0 option if we can't parse it properly
        if (!ptr)
        {
            LogInfo("ProxyCallbackCommon: EDNS0 cannot be parsed for pkt from %#a:%d, ignoring", srcaddr, mDNSVal16(srcport));
        }
        else
        {
            optLen = (int)(ptr - optRR);
            LogInfo("ProxyCallbackCommon: EDNS0 opt length %d present in Question %##s (%s)", optLen, q.qname.c, DNSTypeName(q.qtype));
        }
    }
    else
    {
        LogInfo("ProxyCallbackCommon: EDNS0 opt not present in Question %##s (%s), ptr %p", q.qname.c, DNSTypeName(q.qtype), ptr);
    }
        
    qptr = IsDuplicateClient(srcaddr, srcport, msg->h.id, &q);
    if (qptr)
    {
        LogInfo("ProxyCallbackCommon: Found a duplicate for pkt from %#a:%d, ignoring this", srcaddr, mDNSVal16(srcport));
        return;
    }
    pc = (DNSProxyClient *) mDNSPlatformMemAllocateClear(sizeof(*pc));
    if (!pc)
    {
        LogMsg("ProxyCallbackCommon: Memory failure for pkt from %#a:%d, ignoring this", srcaddr, mDNSVal16(srcport));
        return;
    }
    pc->addr = *srcaddr;
    pc->port = srcport;
    pc->msgid = msg->h.id;
    pc->interfaceID = InterfaceID; // input interface 
    pc->socket = socket;
    pc->tcp = tcp;
    pc->requestFlags = msg->h.flags;
    pc->context = context;
    AssignDomainName(&pc->qname, &q.qname);
    if (optRR)
    {
        if (!ParseEDNS0(pc, optRR, optLen, end))
        {
            LogInfo("ProxyCallbackCommon: Invalid EDNS0 option for pkt from %#a:%d, ignoring this", srcaddr, mDNSVal16(srcport));
        }
        else
        {
            pc->optRR = (mDNSu8 *) mDNSPlatformMemAllocate(optLen);
            if (!pc->optRR)
            {
                LogMsg("ProxyCallbackCommon: Memory failure for pkt from %#a:%d, ignoring this", srcaddr, mDNSVal16(srcport));
                FreeDNSProxyClient(pc);
                return;
            }
            mDNSPlatformMemCopy(pc->optRR, optRR, optLen);
            pc->optLen = optLen;
        }
    }

    debugf("ProxyCallbackCommon: DNS Query forwarding to interface index %d", m->dp_opintf);
    mDNS_SetupQuestion(&pc->q, (mDNSInterfaceID)(unsigned long)m->dp_opintf, &q.qname, q.qtype, ProxyClientCallback, pc);
    pc->q.TimeoutQuestion = 1;
    // Set ReturnIntermed so that we get the negative responses
    pc->q.ReturnIntermed  = mDNStrue;
    pc->q.ProxyQuestion   = mDNStrue;
    pc->q.responseFlags   = zeroID;
#if MDNSRESPONDER_SUPPORTS(APPLE, DNS_PROXY_DNS64)
    pc->qtype = pc->q.qtype;
    if (gDNS64Enabled)
    {
        if (pc->qtype == kDNSType_PTR)
        {
            struct in6_addr v6Addr;
            struct in_addr v4Addr;
            if (GetReverseIPv6Addr(&pc->qname, v6Addr.s6_addr) && nw_nat64_extract_v4(&gDNS64Prefix, &v6Addr, &v4Addr))
            {
                const mDNSu8 *const a = (const mDNSu8 *)&v4Addr.s_addr;
                char qnameStr[MAX_REVERSE_MAPPING_NAME_V4];
                mDNS_snprintf(qnameStr, (mDNSu32)sizeof(qnameStr), "%u.%u.%u.%u.in-addr.arpa.", a[3], a[2], a[1], a[0]);
                MakeDomainNameFromDNSNameString(&pc->q.qname, qnameStr);
                pc->q.qnamehash = DomainNameHashValue(&pc->q.qname);
                pc->dns64state = kDNSProxyDNS64State_PTRSynthesisTrying;
            }
        }
        else if ((pc->qtype == kDNSType_AAAA) && gDNS64ForceAAAASynthesis)
        {
            pc->dns64state = kDNSProxyDNS64State_AAAASynthesis;
            pc->q.qtype    = kDNSType_A;
        }
    }
#endif

    while (*ppc)
        ppc = &((*ppc)->next);
    *ppc = pc;

    mDNS_StartQuery(m, &pc->q);
}

mDNSexport void ProxyUDPCallback(void *socket, DNSMessage *const msg, const mDNSu8 *const end, const mDNSAddr *const srcaddr,
    const mDNSIPPort srcport, const mDNSAddr *dstaddr, const mDNSIPPort dstport, const mDNSInterfaceID InterfaceID, void *context)
{
    LogInfo("ProxyUDPCallback: DNS Message from %#a:%d to %#a:%d length %d", srcaddr, mDNSVal16(srcport), dstaddr, mDNSVal16(dstport), (int)(end - (mDNSu8 *)msg));
    ProxyCallbackCommon(socket, msg, end, srcaddr, srcport, dstaddr, dstport, InterfaceID, mDNSfalse, context);
}

mDNSexport void ProxyTCPCallback(void *socket, DNSMessage *const msg, const mDNSu8 *const end, const mDNSAddr *const srcaddr,
    const mDNSIPPort srcport, const mDNSAddr *dstaddr, const mDNSIPPort dstport, const mDNSInterfaceID InterfaceID, void *context)
{
    LogInfo("ProxyTCPCallback: DNS Message from %#a:%d to %#a:%d length %d", srcaddr, mDNSVal16(srcport), dstaddr, mDNSVal16(dstport), (int)(end - (mDNSu8 *)msg));
    
    // If the connection was closed from the other side or incoming packet does not match stored input interface list, locate the client
    // state and free it.
    if (((end - (mDNSu8 *)msg) == 0) || (!CheckDNSProxyIpIntf(InterfaceID)))
    {
        DNSProxyClient **ppc = &DNSProxyClients;
        DNSProxyClient **prevpc;

        prevpc = ppc;
        while (*ppc && (*ppc)->socket != socket)
        {
            prevpc = ppc;
            ppc=&(*ppc)->next;
        }
        if (!*ppc)
        {
            mDNSPlatformDisposeProxyContext(socket);
            LogMsg("ProxyTCPCallback: socket cannot be found");
            return;
        }
        *prevpc = (*ppc)->next;
        LogInfo("ProxyTCPCallback: free");
        mDNSPlatformDisposeProxyContext(socket);
        FreeDNSProxyClient(*ppc);
        return;
    }
    ProxyCallbackCommon(socket, msg, end, srcaddr, srcport, dstaddr, dstport, InterfaceID, mDNStrue, context);
}

#if MDNSRESPONDER_SUPPORTS(APPLE, DNS_PROXY_DNS64)
mDNSexport void DNSProxyInit(mDNSu32 IpIfArr[MaxIp], mDNSu32 OpIf, const mDNSu8 IPv6Prefix[16], int IPv6PrefixBitLen,
                             mDNSBool forceAAAASynthesis)
#else
mDNSexport void DNSProxyInit(mDNSu32 IpIfArr[MaxIp], mDNSu32 OpIf)
#endif
{
    mDNS *const m = &mDNSStorage;
    int i;

    // Store DNSProxy Interface fields in mDNS struct
    for (i = 0; i < MaxIp; i++)
        m->dp_ipintf[i]  = IpIfArr[i];
    m->dp_opintf         = OpIf;

    LogInfo("DNSProxyInit Storing interface list: Input [%d, %d, %d, %d, %d] Output [%d]", m->dp_ipintf[0],
            m->dp_ipintf[1], m->dp_ipintf[2], m->dp_ipintf[3], m->dp_ipintf[4], m->dp_opintf);
#if MDNSRESPONDER_SUPPORTS(APPLE, DNS_PROXY_DNS64)
    if (IPv6Prefix)
    {
        mDNSu32 copyLen;
        mDNSPlatformMemZero(&gDNS64Prefix, (mDNSu32)sizeof(gDNS64Prefix));
        switch (IPv6PrefixBitLen)
        {
            case 32:
                gDNS64Prefix.length = nw_nat64_prefix_length_32;
                copyLen = 4;
                break;

            case 40:
                gDNS64Prefix.length = nw_nat64_prefix_length_40;
                copyLen = 5;
                break;

            case 48:
                gDNS64Prefix.length = nw_nat64_prefix_length_48;
                copyLen = 6;
                break;

            case 56:
                gDNS64Prefix.length = nw_nat64_prefix_length_56;
                copyLen = 7;
                break;

            case 64:
                gDNS64Prefix.length = nw_nat64_prefix_length_64;
                copyLen = 8;
                break;

            case 96:
                gDNS64Prefix.length = nw_nat64_prefix_length_96;
                copyLen = 12;
                break;

            default:
                copyLen = 0;
                break;
        }
        if (copyLen > 0)
        {
            mDNSPlatformMemCopy(gDNS64Prefix.data, IPv6Prefix, copyLen);
            gDNS64ForceAAAASynthesis = forceAAAASynthesis;
            gDNS64Enabled = mDNStrue;
            LogRedact(MDNS_LOG_CATEGORY_DEFAULT, MDNS_LOG_INFO,
                "DNSProxy using DNS64 IPv6 prefix: " PRI_IPv6_ADDR "/%d" PUB_S,
                IPv6Prefix, IPv6PrefixBitLen, gDNS64ForceAAAASynthesis ? "" : " (force AAAA synthesis)");
        }
        else
        {
            gDNS64Enabled = mDNSfalse;
            gDNS64ForceAAAASynthesis = mDNSfalse;
            LogRedact(MDNS_LOG_CATEGORY_DEFAULT, MDNS_LOG_ERROR,
                "DNSProxy not using invalid DNS64 IPv6 prefix: " PRI_IPv6_ADDR "/%d", IPv6Prefix, IPv6PrefixBitLen);
        }
    }
#endif
}

mDNSexport void DNSProxyTerminate(void)
{
    mDNS *const m = &mDNSStorage;
    int i;
    
    // Clear DNSProxy Interface fields from mDNS struct
    for (i = 0; i < MaxIp; i++)
        m->dp_ipintf[i]  = 0;
    m->dp_opintf         = 0; 
    
    LogInfo("DNSProxyTerminate Cleared interface list: Input [%d, %d, %d, %d, %d] Output [%d]", m->dp_ipintf[0],
            m->dp_ipintf[1], m->dp_ipintf[2], m->dp_ipintf[3], m->dp_ipintf[4], m->dp_opintf);
#if MDNSRESPONDER_SUPPORTS(APPLE, DNS_PROXY_DNS64)
    gDNS64Enabled = mDNSfalse;
#endif
}
#else // UNICAST_DISABLED

mDNSexport void ProxyUDPCallback(void *socket, DNSMessage *const msg, const mDNSu8 *const end, const mDNSAddr *const srcaddr, const mDNSIPPort srcport, const mDNSAddr *dstaddr, const mDNSIPPort dstport, const mDNSInterfaceID InterfaceID, void *context)
{
    (void) socket;
    (void) msg;
    (void) end;
    (void) srcaddr;
    (void) srcport;
    (void) dstaddr;
    (void) dstport;
    (void) InterfaceID;
    (void) context;
}

mDNSexport void ProxyTCPCallback(void *socket, DNSMessage *const msg, const mDNSu8 *const end, const mDNSAddr *const srcaddr, const mDNSIPPort srcport, const mDNSAddr *dstaddr, const mDNSIPPort dstport, const mDNSInterfaceID InterfaceID, void *context)
{
    (void) socket;
    (void) msg;
    (void) end;
    (void) srcaddr;
    (void) srcport;
    (void) dstaddr;
    (void) dstport;
    (void) InterfaceID;
    (void) context;
}

mDNSexport void DNSProxyInit(mDNSu32 IpIfArr[MaxIp], mDNSu32 OpIf)
{
    (void) IpIfArr;
    (void) OpIf;
}
extern void DNSProxyTerminate(void)
{
}


#endif // UNICAST_DISABLED
