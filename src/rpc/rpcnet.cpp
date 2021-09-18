// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The DragonBallChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpc/core/rpcserver.h"

#include "main.h"
#include "net.h"
#include "netbase.h"
#include "p2p/protocol.h"
#include "p2p/node.h"
#include "sync.h"
#include "commons/util/util.h"
#include "tx/blockrewardtx.h"

#include <boost/foreach.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/assign/list_of.hpp>

using namespace boost::assign;
using namespace json_spirit;
using namespace std;

class CNodeMemoryStat {
public:
    uint32_t send_stream_size = 0;
    uint32_t send_msg_count = 0;

    uint32_t recv_get_data_count = 0;
    uint32_t recv_msg_count = 0;

    uint32_t addr_to_count = 0;
    uint32_t addr_known_count = 0;

    uint32_t hash_known_count = 0;
    uint32_t inv_Known_count = 0;
    uint32_t inv_to_send_count = 0;
    uint32_t inv_force_to_send_count = 0;

    uint32_t ask_for_count = 0;
    uint32_t block_confirm_msg_known_count = 0;
    uint32_t block_finality_msg_known_count = 0;

    CNodeMemoryStat(CNode &node);
};

CNodeMemoryStat::CNodeMemoryStat(CNode &node) {
    {
        LOCK(node.cs_vSend);
        send_stream_size = node.ssSend.size();
        send_msg_count   = node.vSendMsg.size();
    }
    {
        LOCK(node.cs_vRecvMsg);
        recv_get_data_count = node.vRecvGetData.size(); // strCommand == "getdata 保存的inv
        recv_msg_count      = node.vRecvMsg.size();
    }

    // static CCriticalSection cs_setBanned;
    // static map<CNetAddr, int64_t> setBanned;

    // flood relay
    addr_to_count    = node.vAddrToSend.size();
    addr_known_count = node.setAddrKnown.size();
    hash_known_count = node.setKnown.size(); // alertHash

    // inventory
    {
        LOCK(node.cs_inventory);
        inv_Known_count         = node.setInventoryKnown.size();
        inv_to_send_count       = node.vInventoryToSend.size();
        inv_force_to_send_count = node.setForceToSend.size();
    }

    ask_for_count = node.mapAskFor.size();

    {
        LOCK(node.cs_blockConfirm);
        block_confirm_msg_known_count = node.setBlockConfirmMsgKnown.size();
    }

    {
        LOCK(node.cs_blockFinality);
        block_finality_msg_known_count = node.setBlockFinalityMsgKnown.size();
    }
}

////////////////////////////////////////////////////////////////////////////////
// class CNodeStats

class CNodeStats {
public:
    NodeId nodeid;
    uint64_t nServices;
    int64_t nLastSend;
    int64_t nLastRecv;
    int64_t nTimeConnected;
    string addrName;
    int32_t nVersion;
    string cleanSubVer;
    bool fInbound;
    int32_t nStartingHeight;
    uint64_t nSendBytes;
    uint64_t nRecvBytes;
    bool fSyncNode;
    double dPingTime;
    double dPingWait;
    string addrLocal;

    // memory stat.
    std::optional<CNodeMemoryStat> mem_stat;

    CNodeStats(CNode &node, bool need_detail);
};

extern CNode* pnodeSync;

CNodeStats::CNodeStats(CNode &node, bool need_detail) {
    nodeid          = node.GetId();
    nServices       = node.nServices;
    nLastSend       = node.nLastSend;
    nLastRecv       = node.nLastRecv;
    nTimeConnected  = node.nTimeConnected;
    addrName        = node.addrName;
    nVersion        = node.nVersion;
    cleanSubVer     = node.cleanSubVer;
    fInbound        = node.fInbound;
    nStartingHeight = node.nStartingHeight;
    nSendBytes      = node.nSendBytes;
    nRecvBytes      = node.nRecvBytes;
    fSyncNode       = (&node == pnodeSync);

    // It is common for nodes with good ping times to suddenly become lagged,
    // due to a new block arriving or other large transfer.
    // Merely reporting pingtime might fool the caller into thinking the node was still responsive,
    // since pingtime does not update until the ping is complete, which might take a while.
    // So, if a ping is taking an unusually long time in flight,
    // the caller can immediately detect that this is happening.
    int64_t nPingUsecWait = 0;
    if ((0 != node.nPingNonceSent) && (0 != node.nPingUsecStart)) {
        nPingUsecWait = GetTimeMicros() - node.nPingUsecStart;
    }

    // Raw ping time is in microseconds, but show it to user as whole seconds (Coin users should be well used to small
    // numbers with many decimal places by now :)
    dPingTime = (((double)node.nPingUsecTime) / 1e6);
    dPingWait = (((double)nPingUsecWait) / 1e6);

    // Leave string empty if addrLocal invalid (not filled in yet)
    addrLocal = node.addrLocal.IsValid() ? node.addrLocal.ToString() : "";
    if (need_detail) {
        mem_stat.emplace(node);
    }
}

Value getconnectioncount(const Array& params, bool fHelp) {
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getconnectioncount\n"
            "\nReturns the number of connections to other nodes.\n"
            "\nbResult:\n"
            "n          (numeric) The connection count\n"
            "\nExamples:\n" +
            HelpExampleCli("getconnectioncount", "") + "\nAs json rpc\n" + HelpExampleRpc("getconnectioncount", ""));

    LOCK(cs_vNodes);
    return (int32_t)vNodes.size();
}

Value ping(const Array& params, bool fHelp) {
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "ping\n"
            "\nRequests that a ping be sent to all other nodes, to measure ping time.\n"
            "Results provided in getpeerinfo, pingtime and pingwait fields are decimal seconds.\n"
            "Ping command is handled in queue with all other commands, so it measures processing backlog, not just "
            "network ping.\n"
            "\nExamples:\n" +
            HelpExampleCli("ping", "") + "\nAs json rpc\n" + HelpExampleRpc("ping", ""));

    // Request that each node send a ping during next message processing pass
    LOCK(cs_vNodes);
    for (auto pNode : vNodes) {
        pNode->fPingQueued = true;
    }

    return Value::null;
}

static void CopyNodeStats(vector<CNodeStats>& vstats, bool needDetail) {
    vstats.clear();

    LOCK(cs_vNodes);
    vstats.reserve(vNodes.size());
    for(auto pNode : vNodes) {
        vstats.push_back(CNodeStats(*pNode, needDetail));
    }
}

Value getpeerinfo(const Array& params, bool fHelp) {
    if (fHelp || (params.size() != 0 && params.size() != 1))
        throw runtime_error(
            "getpeerinfo\n"
            "\nReturns data about each connected network node as a json array of objects.\n"
            "\nArguments:\n"
            "1. \"detail\"     (boolean, optional) show detail (false)\n"
            "\nbResult:\n"
            "[\n"
            "  {\n"
            "    \"addr\":\"host:port\",      (string) The ip address and port of the peer\n"
            "    \"addrlocal\":\"ip:port\",   (string) local address\n"
            "    \"services\":\"00000001\",   (string) The services\n"
            "    \"lastsend\": ttt,           (numeric) The time in seconds since epoch (Jan 1 1970 GMT) of the last send\n"
            "    \"lastrecv\": ttt,           (numeric) The time in seconds since epoch (Jan 1 1970 GMT) of the last receive\n"
            "    \"bytessent\": n,            (numeric) The total bytes sent\n"
            "    \"bytesrecv\": n,            (numeric) The total bytes received\n"
            "    \"conntime\": ttt,           (numeric) The connection time in seconds since epoch (Jan 1 1970 GMT)\n"
            "    \"pingtime\": n,             (numeric) ping time\n"
            "    \"pingwait\": n,             (numeric) ping wait\n"
            "    \"version\": v,              (numeric) The peer version, such as 7001\n"
            "    \"subver\": \"/Satoshi:0.8.5/\",  (string) The string version\n"
            "    \"inbound\": true|false,     (boolean) Inbound (true) or Outbound (false)\n"
            "    \"startingheight\": n,       (numeric) The starting height (block) of the peer\n"
            "    \"banscore\": n,             (numeric) The ban score (stats.nMisbehavior)\n"
            "    \"syncnode\" : true|false    (boolean) if sync node\n"
            "  }\n"
            "  ,...\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getpeerinfo", "")
            + HelpExampleRpc("getpeerinfo", "")
        );
    bool needDetail = params.size() > 0 ? params[0].get_bool() : false;

    vector<CNodeStats> vstats;
    CopyNodeStats(vstats, needDetail);

    Array ret;

    for(const CNodeStats& stats: vstats) {
        Object obj;
        CNodeStateStats statestats;
        bool fStateStats = GetNodeStateStats(stats.nodeid, statestats);
        obj.push_back(Pair("addr",          stats.addrName));

        if (!(stats.addrLocal.empty()))
            obj.push_back(Pair("addrlocal", stats.addrLocal));

        obj.push_back(Pair("services",      strprintf("%08x", stats.nServices)));
        obj.push_back(Pair("lastsend",      stats.nLastSend));
        obj.push_back(Pair("lastrecv",      stats.nLastRecv));
        obj.push_back(Pair("bytessent",     stats.nSendBytes));
        obj.push_back(Pair("bytesrecv",     stats.nRecvBytes));
        obj.push_back(Pair("conntime",      stats.nTimeConnected));
        obj.push_back(Pair("pingtime",      stats.dPingTime));

        if (stats.dPingWait > 0.0)
            obj.push_back(Pair("pingwait",  stats.dPingWait));

        obj.push_back(Pair("version",       stats.nVersion));
        // Use the sanitized form of subver here, to avoid tricksy remote peers from
        // corrupting or modifying the JSON output by putting special characters in
        // their ver message.
        obj.push_back(Pair("subver",        stats.cleanSubVer));
        obj.push_back(Pair("inbound",       stats.fInbound));
        obj.push_back(Pair("startingheight",stats.nStartingHeight));

        if (fStateStats) {
            obj.push_back(Pair("banscore",  statestats.nMisbehavior));
        }

        obj.push_back(Pair("syncnode",      stats.fSyncNode));

        if (needDetail) {
            Object detailObj;
            auto &memDetail = *stats.mem_stat;
            detailObj.push_back(Pair("send_stream_size", memDetail.send_stream_size));
            detailObj.push_back(Pair("send_msg_count", memDetail.send_msg_count));
            detailObj.push_back(Pair("recv_get_data_count", memDetail.recv_get_data_count));
            detailObj.push_back(Pair("recv_msg_count", memDetail.recv_msg_count));
            detailObj.push_back(Pair("addr_to_count", memDetail.addr_to_count));
            detailObj.push_back(Pair("addr_known_count", memDetail.addr_known_count));
            detailObj.push_back(Pair("hash_known_count", memDetail.hash_known_count));
            detailObj.push_back(Pair("inv_Known_count", memDetail.inv_Known_count));
            detailObj.push_back(Pair("inv_to_send_count", memDetail.inv_to_send_count));
            detailObj.push_back(Pair("inv_force_to_send_count", memDetail.inv_force_to_send_count));
            detailObj.push_back(Pair("ask_for_count", memDetail.ask_for_count));
            detailObj.push_back(Pair("block_confirm_msg_known_count", memDetail.block_confirm_msg_known_count));
            detailObj.push_back(Pair("block_finality_msg_known_count", memDetail.block_finality_msg_known_count));

            obj.push_back(Pair("detail",      detailObj));
        }

        ret.push_back(obj);
    }

    return ret;
}

Value addnode(const Array& params, bool fHelp) {
    string strCommand;
    if (params.size() == 2)
        strCommand = params[1].get_str();
    if (fHelp || params.size() != 2 || (strCommand != "onetry" && strCommand != "add" && strCommand != "remove"))
        throw runtime_error(
            "addnode \"node:port\" \"add|remove|onetry\"\n"
            "\nAttempts add or remove a node from the addnode list.\n"
            "Or try a connection to a node once.\n"
            "\nArguments:\n"
            "1. \"node:port\"     (string, required) The node IP and port (see getpeerinfo for nodes)\n"
            "2. \"command\"  (string, required) 'add' to add a node to the list, 'remove' to remove a node from the "
            "list, 'onetry' to try a connection to the node once\n"
            "\nResult:\n"
            "\nExamples:\n" +
            HelpExampleCli("addnode", "\"192.168.0.6:8333\" onetry") + "\nAs json rpc\n" +
            HelpExampleRpc("addnode", "\"192.168.0.6:8333\", onetry"));

    RPCTypeCheck(params, boost::assign::list_of(str_type)(str_type));

    string strNode = params[0].get_str();
    if (strNode.find("127.0.0.1:") != std::string::npos)
        throw JSONRPCError(RPC_CLIENT_IS_LOCALHOST_ERROR, "Error: Node can't be a localhost.");

    if (strCommand == "onetry") {
        CAddress addr;
        ConnectNode(addr, strNode.c_str());
        return Value::null;
    }

    LOCK(cs_vAddedNodes);
    vector<string>::iterator it = vAddedNodes.begin();
    for(; it != vAddedNodes.end(); it++)
        if (strNode == *it)
            break;

    if (strCommand == "add") {
        if (it != vAddedNodes.end())
            throw JSONRPCError(RPC_CLIENT_NODE_ALREADY_ADDED, "Error: Node already added");
        vAddedNodes.push_back(strNode);
    } else if(strCommand == "remove") {
        if (it == vAddedNodes.end())
            throw JSONRPCError(RPC_CLIENT_NODE_NOT_ADDED, "Error: Node not added before.");
        vAddedNodes.erase(it);
    }

    return Value::null;
}

Value getaddednodeinfo(const Array& params, bool fHelp) {
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getaddednodeinfo \"dns\" [\"node\"]\n"
            "\nReturns information about the given added node, or all added nodes\n"
            "(note that onetry addnodes are not listed here)\n"
            "If dns is false, only a list of added nodes will be provided,\n"
            "otherwise connected information will also be available.\n"
            "\nArguments:\n"
            "1.\"dns\"      (boolean, required) If false, only a list of added nodes will be provided, otherwise "
            "connected information will also be available.\n"
            "2.\"node\"     (string, optional) If provided, return information about this specific node, otherwise all "
            "nodes are returned.\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"addednode\" : \"192.168.0.201\",   (string) The node ip address\n"
            "    \"connected\" : true|false,          (boolean) If connected\n"
            "    \"addresses\" : [\n"
            "       {\n"
            "         \"address\" : \"192.168.0.201:8333\",  (string) The Coin server host and port\n"
            "         \"connected\" : \"outbound\"           (string) connection, inbound or outbound\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"
            "  }\n"
            "  ,...\n"
            "]\n"
            "\nExamples:\n" +
            HelpExampleCli("getaddednodeinfo", "true \"192.168.0.201\"") + "\nAs json rpc\n" +
            HelpExampleRpc("getaddednodeinfo", "true, \"192.168.0.201\""));

    bool fDns = params[0].get_bool();

    list<string> addedNodes;
    if (params.size() == 1) {
        LOCK(cs_vAddedNodes);
        for (auto& strAddNode : vAddedNodes)
            addedNodes.push_back(strAddNode);
    } else {
        string strNode = params[1].get_str();
        LOCK(cs_vAddedNodes);
        for (auto& strAddNode : vAddedNodes)
            if (strAddNode == strNode) {
                addedNodes.push_back(strAddNode);
                break;
            }
        if (addedNodes.size() == 0)
            throw JSONRPCError(RPC_CLIENT_NODE_NOT_ADDED, "Error: Node has not been added.");
    }

    Array ret;
    if (!fDns) {
        for (auto& strAddNode : addedNodes) {
            Object obj;
            obj.push_back(Pair("addednode", strAddNode));
            ret.push_back(obj);
        }
        return ret;
    }

    list<pair<string, vector<CService> > > addedAddresses;
    for (auto& strAddNode : addedNodes) {
        vector<CService> vservNode(0);
        if (Lookup(strAddNode.c_str(), vservNode, SysCfg().GetDefaultPort(), fNameLookup, 0))
            addedAddresses.push_back(make_pair(strAddNode, vservNode));
        else {
            Object obj;
            obj.push_back(Pair("addednode", strAddNode));
            obj.push_back(Pair("connected", false));
            Array addresses;
            obj.push_back(Pair("addresses", addresses));
        }
    }

    LOCK(cs_vNodes);
    for (list<pair<string, vector<CService> > >::iterator it = addedAddresses.begin(); it != addedAddresses.end();
         it++) {
        Object obj;
        obj.push_back(Pair("addednode", it->first));

        Array addresses;
        bool fConnected = false;
        for (auto& addrNode : it->second) {
            bool fFound = false;
            Object node;
            node.push_back(Pair("address", addrNode.ToString()));
            for (auto pNode : vNodes)
                if (pNode->addr == addrNode) {
                    fFound     = true;
                    fConnected = true;
                    node.push_back(Pair("connected", pNode->fInbound ? "inbound" : "outbound"));
                    break;
                }
            if (!fFound) node.push_back(Pair("connected", "false"));
            addresses.push_back(node);
        }
        obj.push_back(Pair("connected", fConnected));
        obj.push_back(Pair("addresses", addresses));
        ret.push_back(obj);
    }

    return ret;
}

Value getnettotals(const Array& params, bool fHelp) {
    if (fHelp || params.size() > 0)
        throw runtime_error(
            "getnettotals\n"
            "\nReturns information about network traffic, including bytes in, bytes out,\n"
            "and current time.\n"
            "\nResult:\n"
            "{\n"
            "  \"totalbytesrecv\": n,   (numeric) Total bytes received\n"
            "  \"totalbytessent\": n,   (numeric) Total bytes sent\n"
            "  \"timemillis\": t        (numeric) Total cpu time\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("getnettotals", "") + "\nAs json rpc\n" + HelpExampleRpc("getnettotals", ""));

    Object obj;
    obj.push_back(Pair("totalbytesrecv",    CNode::GetTotalBytesRecv()));
    obj.push_back(Pair("totalbytessent",    CNode::GetTotalBytesSent()));
    obj.push_back(Pair("timemillis",        GetTimeMillis()));
    return obj;
}

Value getnetworkinfo(const Array& params, bool fHelp) {
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getnetworkinfo\n"
            "\nget various information about network.\n"
            "Returns an object containing various state info regarding P2P networking.\n"
            "\nResult:\n"
            "{\n"
            "  \"version\": xxxxx,           (numeric) the server version\n"
            "  \"protocolversion\": xxxxx,   (numeric) the protocol version\n"
            "  \"timeoffset\": xxxxx,        (numeric) the time offset\n"
            "  \"connections\": xxxxx,       (numeric) the number of connections\n"
            "  \"proxy\": \"host:port\",     (string, optional) the proxy used by the server\n"
            "  \"relayfee\": x.xxxx,         (numeric) minimum relay fee for non-free transactions in btc/kb\n"
            "  \"localaddresses\": [,        (array) list of local addresses\n"
            "    \"address\": \"xxxx\",      (string) network address\n"
            "    \"port\": xxx,              (numeric) network port\n"
            "    \"score\": xxx              (numeric) relative score\n"
            "  ]\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("getnetworkinfo", "") + "\nAs json rpc\n" + HelpExampleRpc("getnetworkinfo", ""));

    ProxyType proxy;
    GetProxy(NET_IPV4, proxy);

    Object obj;
    obj.push_back(Pair("version",           (int32_t)CLIENT_VERSION));
    obj.push_back(Pair("protocolversion",   (int32_t)PROTOCOL_VERSION));
    obj.push_back(Pair("timeoffset",        GetTimeOffset()));
    obj.push_back(Pair("connections",       (int32_t)vNodes.size()));
    obj.push_back(Pair("proxy",             (proxy.first.IsValid() ? proxy.first.ToStringIPPort() : string())));
    obj.push_back(Pair("relayfee",          JsonValueFromAmount(MIN_RELAY_TX_FEE)));
    Array localAddresses;
    {
        LOCK(cs_mapLocalHost);
        for(const auto &item: mapLocalHost)
        {
            Object rec;
            rec.push_back(Pair("address",   item.first.ToString()));
            rec.push_back(Pair("port",      item.second.nPort));
            rec.push_back(Pair("score",     item.second.nScore));
            localAddresses.push_back(rec);
        }
    }
    obj.push_back(Pair("localaddresses",    localAddresses));
    return obj;
}

Value getchaininfo(const Array& params, bool fHelp) {
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getchaininfo \"count\" [height]\n"
            "\nget the chain state of the most recent blocks.\n"
            "\nArguments:\n"
            "1.\"count\":                 (numeric, required) The count of the most recent blocks to get. MAX=10000\n"
            "2.\"height\":              (numeric, optional) The tip height of blocks\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"height\": n,         (numeric) The block height\n"
            "    \"time\": n,           (numeric) The block time\n"
            "    \"tx_count\":n,        (numeric) The transaction number in the block\n"
            "    \"fuel\": n,           (numeric) The fuel consumed in the block\n"
            "    \"fuel_rate\":n,       (numeric) The fuel rate in the block\n"
            "    \"miner\": n,          (string) The miner\n"
            "  },\n"
            "  ...\n"
            "]\n"
            "\nExamples:\n" +
            HelpExampleCli("getchaininfo", "5") + "\nAs json rpc call\n" + HelpExampleRpc("getchaininfo", "5"));

    int32_t count = params[0].get_int();
    int32_t height = chainActive.Height();
    if (params.size() > 1) {
        height = params[1].get_int();
        if (height > chainActive.Height()) {
            throw JSONRPCError(RPC_INVALID_PARAMS, strprintf("The height exceed the tip height! height=%d, tip_height=%d",
                height, chainActive.Height()));
        }
    }
    if (count < 1 || count > height || count > MAX_RECENT_BLOCK_COUNT)
        throw JSONRPCError(RPC_INVALID_PARAMS, strprintf("The input count out of range! count=%d, height=%d, max_count=%d",
            count, height, MAX_RECENT_BLOCK_COUNT));

    CBlockIndex* pBlockIndex = chainActive[height];
    Array array;
    CBlock block;

    for (int32_t i = 0; (i < count) && (pBlockIndex != nullptr); i++) {
        Object object;
        CDiskBlockIndex diskBlockIndex;
        if (!pCdMan->pBlockIndexDb->GetBlockIndex(pBlockIndex->GetBlockHash(), diskBlockIndex)) {
            throw JSONRPCError(RPC_INVALID_PARAMS,
                strprintf("the index of block=%s not found in db", pBlockIndex->GetIdString()));
        }
        object.push_back(Pair("height",     pBlockIndex->height));
        object.push_back(Pair("time",       pBlockIndex->GetBlockTime()));
        object.push_back(Pair("tx_count",   (int32_t)diskBlockIndex.nTx));
        object.push_back(Pair("fuel_fee",   (int64_t)diskBlockIndex.nFuelFee));
        object.push_back(Pair("fuel_rate",  (int32_t)diskBlockIndex.nFuelRate));

        block.SetNull();
        if (ReadBlockFromDisk(pBlockIndex, block)) {
            object.push_back(Pair("miner",  block.vptx[0]->txUid.ToString()));
        }

        array.push_back(object);

        pBlockIndex = pBlockIndex->pprev;
    }

    return array;
}
