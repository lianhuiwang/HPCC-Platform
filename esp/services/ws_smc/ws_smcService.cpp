/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#pragma warning (disable : 4786)

#include "build-config.h"

#ifdef _USE_OPENLDAP
#include "ldapsecurity.ipp"
#endif
#include "ws_smcService.hpp"
#include "wshelpers.hpp"

#include "dalienv.hpp"
#include "WUWrapper.hpp"
#include "wujobq.hpp"
#include "dfuwu.hpp"
#include "exception_util.hpp"

#include "roxiecontrol.hpp"

static const char* FEATURE_URL = "SmcAccess";
const char* THORQUEUE_FEATURE = "ThorQueueAccess";
static const char* ROXIE_CONTROL_URL = "RoxieControlAccess";

const char* PERMISSIONS_FILENAME = "espsmc_permissions.xml";

void AccessSuccess(IEspContext& context, char const * msg,...) __attribute__((format(printf, 2, 3)));
void AccessSuccess(IEspContext& context, char const * msg,...)
{
    StringBuffer buf;
    buf.appendf("User %s: ",context.queryUserId());
    va_list args;
    va_start(args, msg);
    buf.valist_appendf(msg, args);
    va_end(args);
    AUDIT(AUDIT_TYPE_ACCESS_SUCCESS,buf.str());
}

void AccessFailure(IEspContext& context, char const * msg,...) __attribute__((format(printf, 2, 3)));
void AccessFailure(IEspContext& context, char const * msg,...)
{
    StringBuffer buf;
    buf.appendf("User %s: ",context.queryUserId());
    va_list args;
    va_start(args, msg);
    buf.valist_appendf(msg, args);
    va_end(args);

    AUDIT(AUDIT_TYPE_ACCESS_FAILURE,buf.str());
}

struct QueueWrapper
{
    QueueWrapper(const char* targetName, const char* queueExt)
    {
        StringBuffer name;
        name.append(targetName).append('.').append(queueExt);
        queue.setown(createJobQueue(name.str()));
    }

    QueueWrapper(const char* queueName)
    {
        queue.setown(createJobQueue(queueName));
    }

    operator IJobQueue*() { return queue.get(); }
    IJobQueue* operator->() { return queue.get(); }

    Owned<IJobQueue> queue;
};

struct QueueLock
{
    QueueLock(IJobQueue* q): queue(q) { queue->lock(); }
    ~QueueLock() 
    { 
        queue->unlock(); 
    }

    Linked<IJobQueue> queue;
};

void CWsSMCEx::init(IPropertyTree *cfg, const char *process, const char *service)
{
    if (!daliClientActive())
    {
        ERRLOG("No Dali Connection Active.");
        throw MakeStringException(-1, "No Dali Connection Active. Please Specify a Dali to connect to in you configuration file");
    }

    m_BannerAction = 0;
    m_EnableChatURL = false;
    m_BannerSize = "4";
    m_BannerColor = "red";
    m_BannerScroll = "2";

    StringBuffer xpath;
    xpath.appendf("Software/EspProcess[@name='%s']/@portalurl", process);
    const char* portalURL = cfg->queryProp(xpath.str());
    if (portalURL && *portalURL)
        m_PortalURL.append(portalURL);
}

static void countProgress(IPropertyTree *t,unsigned &done,unsigned &total)
{
    total = 0;
    done = 0;
    Owned<IPropertyTreeIterator> it = t->getElements("DFT/progress");
    ForEach(*it) {
        IPropertyTree &e=it->query();
        if (e.getPropInt("@done",0))
            done++;
        total++;
    }
}

struct CActiveWorkunitWrapper: public CActiveWorkunit
{
    CActiveWorkunitWrapper(IEspContext &context, const char* wuid,unsigned index=0): CActiveWorkunit("","")
    {
        double version = context.getClientVersion();

        CWUWrapper wu(wuid, context);
        StringBuffer stateStr;
        SCMStringBuffer state,owner,jobname;
        setWuid(wuid);
        wu->getStateDesc(state);
        if(index)
            stateStr.appendf("queued(%d) [%s]", index, state.str());
        else
            stateStr.set(state.str());

        setState(stateStr.str());
        setStateID(wu->getState());
        if ((version > 1.09) && (wu->getState() == WUStateFailed))
            setWarning("The job will ultimately not complete. Please check ECLAgent.");

        setOwner(wu->getUser(owner).str());
        setJobname(wu->getJobName(jobname).str());
        switch(wu->getPriority())
        {
            case PriorityClassHigh: setPriority("high"); break;
            default:
            case PriorityClassNormal: setPriority("normal"); break;
            case PriorityClassLow: setPriority("low"); break;
        }

        if (version > 1.08 && wu->isPausing())
        {
            setIsPausing(true);
        }
        if (version > 1.14)
        {
            SCMStringBuffer clusterName;
            setClusterName(wu->getClusterName(clusterName).str());
        }
    }

    CActiveWorkunitWrapper(const char* wuid,const char* owner, const char* jobname, const char* state, const char* priority): CActiveWorkunit("","")
    {
        setWuid(wuid);
        setState(state);
        setOwner(owner);
        setJobname(jobname);
        setPriority(priority);
    }
};

bool CWsSMCEx::onIndex(IEspContext &context, IEspSMCIndexRequest &req, IEspSMCIndexResponse &resp)
{
    resp.setRedirectUrl("/");
    return true;
}

static int stringcmp(const char **a, const char **b)
{
    return strcmp(*a, *b);
}

bool isInWuList(IArrayOf<IEspActiveWorkunit>& aws, const char* wuid)
{
    bool bFound = false;
    if (wuid && *wuid && (aws.length() > 0))
    {
        ForEachItemIn(k, aws)
        {
            IEspActiveWorkunit& wu = aws.item(k);
            const char* wuid0 = wu.getWuid();
            const char* server0 = wu.getServer();
            if (wuid0 && !strcmp(wuid0, wuid) && (!server0 || strcmp(server0, "ECLagent")))
            {
                bFound = true;
                break;
            }
        }
    }

    return bFound;
}

void addQueuedWorkUnits(const char *queueName, CJobQueueContents &contents, IArrayOf<IEspActiveWorkunit> &aws, IEspContext &context, const char *serverName, const char *instanceName)
{
    Owned<IJobQueueIterator> iter = contents.getIterator();
    unsigned count=0;
    ForEach(*iter)
    {
        if (!isInWuList(aws, iter->query().queryWUID()))
        {
            try
            {
                    Owned<IEspActiveWorkunit> wu(new CActiveWorkunitWrapper(context, iter->query().queryWUID(),++count));
                    wu->setServer(serverName);
                    wu->setInstance(instanceName); // JCSMORE In thor case at least, if queued it is unknown which instance it will run on..
                    wu->setQueueName(queueName);

                    aws.append(*wu.getLink());
            }
            catch (IException *e)
            {
                // JCSMORE->KWang what is this handling? Why would this succeeed and above fail?
                StringBuffer msg;
                Owned<IEspActiveWorkunit> wu(new CActiveWorkunitWrapper(iter->query().queryWUID(), "", "", e->errorMessage(msg).str(), "normal"));
                wu->setServer(serverName);
                wu->setInstance(instanceName);
                wu->setQueueName(queueName);

                aws.append(*wu.getLink());
            }
        }
    }
}

void CWsSMCEx::getQueueState(int runningJobsInQueue, StringBuffer& queueState, BulletType& bulletType)
{
    bool queuePausedOrStopped = false;
    if ((queueState.length() > 0) && (strieq(queueState.str(),"stopped") || strieq(queueState.str(),"paused")))
        queuePausedOrStopped = true;
    else
        queueState.set("running");

    bulletType = bulletGreen;
    if (NotFound == runningJobsInQueue)
    {
        if (queuePausedOrStopped)
            bulletType = bulletWhite;
        else
            bulletType = bulletError;
    }
    else if (runningJobsInQueue > 0)
    {
        if (queuePausedOrStopped)
            bulletType = bulletOrange;
        else
            bulletType = bulletGreen;
    }
    else if (queuePausedOrStopped)
        bulletType = bulletYellow;

    return;
}

void CWsSMCEx::readClusterTypeAndQueueName(CConstWUClusterInfoArray& clusters, const char* clusterName, StringBuffer& clusterType, SCMStringBuffer& clusterQueue)
{
    if (!clusterName || !*clusterName)
        return;

    ForEachItemIn(cl, clusters)
    {
        IConstWUClusterInfo &cluster = clusters.item(cl);
        SCMStringBuffer str;
        cluster.getName(str);
        if (!streq(str.str(), clusterName))
            continue;

        if (cluster.getPlatform() == HThorCluster)
        {
            cluster.getAgentQueue(clusterQueue);
            clusterType.set("HThor");
        }
        else if (cluster.getPlatform() == RoxieCluster)
        {
            cluster.getAgentQueue(clusterQueue);
            clusterType.set("Roxie");
        }
        else
        {
            cluster.getThorQueue(clusterQueue);
            clusterType.set("Thor");
        }
        break;
    }

    return;
}

void CWsSMCEx::addRunningWUs(IEspContext &context, IPropertyTree& node, CConstWUClusterInfoArray& clusters,
                   IArrayOf<IEspActiveWorkunit>& aws, BoolHash& uniqueWUIDs,
                   StringArray& runningQueueNames, int* runningJobsInQueue)
{
    StringBuffer instance;
    StringBuffer qname;
    int serverID = -1;
    const char* name = node.queryProp("@name");
    if (name && *name)
    {
        node.getProp("@queue", qname);
        if (0 == stricmp("ThorMaster", name))
        {
            node.getProp("@thorname",instance);
        }
        else if (0 == stricmp(name, "ECLAgent"))
        {
            qname.append(name);
        }
        if ((instance.length()==0))
        {
            instance.append( !strcmp(name, "ECLagent") ? "ECL agent" : name);
            instance.append(" on ").append(node.queryProp("@node"));
        }
    }
    if (qname.length() > 0)
    {
        StringArray qlist;
        qlist.appendListUniq(qname.str(), ",");
        ForEachItemIn(q, qlist)
        {
            const char *_qname = qlist.item(q);
            serverID = runningQueueNames.find(_qname);
            if (NotFound == serverID)
            {
                serverID = runningQueueNames.ordinality(); // i.e. last
                runningQueueNames.append(_qname);
            }
        }
    }
    Owned<IPropertyTreeIterator> wuids(node.getElements("WorkUnit"));
    ForEach(*wuids)
    {
        const char* wuid=wuids->query().queryProp(NULL);
        if (!wuid)
            continue;

        if (streq(qname.str(), "ECLagent") && uniqueWUIDs.getValue(wuid))
            continue;

        try
        {
            IEspActiveWorkunit* wu=new CActiveWorkunitWrapper(context,wuid);
            const char* servername = node.queryProp("@name");
            const char *cluster = node.queryProp("Cluster");
            wu->setServer(servername);
            wu->setInstance(instance.str());
            StringBuffer queueName;
            if (cluster) // backward compat check.
                getClusterThorQueueName(queueName, cluster);
            else
                queueName.append(qname);
            serverID = runningQueueNames.find(queueName.str());
            wu->setQueueName(queueName);
            double version = context.getClientVersion();
            if (version > 1.01)
            {
                if (wu->getStateID() == WUStateRunning)
                {
                    int sg_duration = node.getPropInt("@sg_duration", -1);
                    const char* graph = node.queryProp("@graph");
                    int subgraph = node.getPropInt("@subgraph", -1);
                    if (subgraph > -1 && sg_duration > -1)
                    {
                        StringBuffer durationStr;
                        StringBuffer subgraphStr;
                        durationStr.appendf("%d min", sg_duration);
                        subgraphStr.appendf("%d", subgraph);
                        wu->setGraphName(graph);
                        wu->setDuration(durationStr.str());
                        wu->setGID(subgraphStr.str());
                    }
                    int memoryBlocked = node.getPropInt("@memoryBlocked ", 0);
                    if (memoryBlocked != 0)
                    {
                        wu->setMemoryBlocked(1);
                    }

                    if (serverID > -1)
                    {
                        runningJobsInQueue[serverID]++;
                    }

                    if ((version > 1.14) && streq(queueName.str(), "ECLagent"))
                    {
                        const char* clusterName = wu->getClusterName();
                        if (clusterName && *clusterName)
                        {
                            StringBuffer clusterType;
                            SCMStringBuffer clusterQueue;
                            readClusterTypeAndQueueName(clusters, clusterName, clusterType, clusterQueue);
                            wu->setClusterType(clusterType.str());
                            wu->setClusterQueueName(clusterQueue.str());
                        }
                    }
                }
            }

            uniqueWUIDs.setValue(wuid, true);
            aws.append(*wu);
        }
        catch (IException *e)
        {
            StringBuffer msg;
            Owned<IEspActiveWorkunit> wu(new CActiveWorkunitWrapper(wuid, "", "", e->errorMessage(msg).str(), "normal"));
            wu->setServer(node.queryProp("@name"));
            wu->setInstance(instance.str());
            wu->setQueueName(qname.str());

            aws.append(*wu.getLink());
        }
    }

    return;
}

// This method reads job information from both /Status/Servers and IJobQueue.
//
// Each server component (a thor cluster, a dfuserver, or an eclagent) is one 'Server' branch under
// /Status/Servers. A 'Server' branch has a @queue which indicates the queue name of the server.
// A 'Server' branch also contains the information about running WUs on that 'Server'. This
// method reads the information. Those WUs are displays under that server (identified by its queue name)
// on Activity page.
//
// For the WUs list inside /Status/Servers/Server[@name=ECLagent] but not list under other 'Server', the
// existing code has to find out WUID and @clusterName of the WU. Then, uses @clusterName to find out the
// queue name in IConstWUClusterInfo. Those WUs list under that server (identified by its queue name) with
// a note 'on ECLagent'. TBD: the logic here will be simpler if the /Status/Servers/Server is named the
// instance and/or cluster.
//
// In order to get information about queued WUs, this method gets queue names from both IConstWUClusterInfo
// and other environment functions. Each of those queue names is linked to one IJobQueues. From the
// IJobQueues, this method reads queued jobs for each server component and list them under the server
// component (identified by its queue name).

bool CWsSMCEx::onActivity(IEspContext &context, IEspActivityRequest &req, IEspActivityResponse& resp)
{
    context.validateFeatureAccess(FEATURE_URL, SecAccess_Read, true);

    try
    {
        const char* build_ver = getBuildVersion();
        resp.setBuild(build_ver);

        double version = context.getClientVersion();

        bool isSuperUser = true;
#ifdef _USE_OPENLDAP
        CLdapSecManager* secmgr = dynamic_cast<CLdapSecManager*>(context.querySecManager());
        if(secmgr && !secmgr->isSuperUser(context.queryUser()))
            isSuperUser =  false;
#endif
        if(isSuperUser && req.getFromSubmitBtn())
        {
            StringBuffer chatURLStr, bannerStr;
            const char* chatURL = req.getChatURL();
            const char* banner = req.getBannerContent();
            //Only display valid strings
            if (chatURL)
            {
                const char* pStr = chatURL;
                for (unsigned i = 0; i < strlen(chatURL); i++)
                {
                    if ((pStr[0] > 31) && (pStr[0] < 127))
                        chatURLStr.append(pStr[0]);
                    pStr++;
                }
            }
            if (banner)
            {
                const char* pStr = banner;
                for (unsigned i = 0; i < strlen(banner); i++)
                {
                    if ((pStr[0] > 31) && (pStr[0] < 127))
                        bannerStr.append(pStr[0]);
                    pStr++;
                }
            }
            chatURLStr.trim();
            bannerStr.trim();

            if (!req.getBannerAction_isNull() && req.getBannerAction() && (bannerStr.length() < 1))
            {
                throw MakeStringException(ECLWATCH_MISSING_BANNER_CONTENT, "If a Banner is enabled, the Banner content must be specified.");
            }

            if (!req.getEnableChatURL_isNull() && req.getEnableChatURL() && (chatURLStr.length() < 1))
            {
                throw MakeStringException(ECLWATCH_MISSING_CHAT_URL, "If a Chat is enabled, the Chat URL must be specified.");
            }

            m_ChatURL = chatURLStr;
            m_Banner = bannerStr;

            const char* bannerSize = req.getBannerSize();
            if (bannerSize && *bannerSize)
                m_BannerSize.clear().append(bannerSize);

            const char* bannerColor = req.getBannerColor();
            if (bannerColor && *bannerColor)
                m_BannerColor.clear().append(bannerColor);

            const char* bannerScroll = req.getBannerScroll();
            if (bannerScroll && *bannerScroll)
                m_BannerScroll.clear().append(bannerScroll);

            m_BannerAction = req.getBannerAction();
            if(!req.getEnableChatURL_isNull())
                m_EnableChatURL = req.getEnableChatURL();
        }

        if (version > 1.05)
        {
            if (version > 1.11)
                resp.setSuperUser(isSuperUser);
            resp.setShowBanner(m_BannerAction);
            resp.setShowChatURL(m_EnableChatURL);
            resp.setBannerContent(m_Banner.str());
            resp.setBannerSize(m_BannerSize.str());
            resp.setBannerColor(m_BannerColor.str());
            resp.setChatURL(m_ChatURL.str());

            if (version > 1.07)
            {
                resp.setBannerScroll(m_BannerScroll.str());
            }
        }

        Owned<IRemoteConnection> connEnv = querySDS().connect("Environment", myProcessSession(), RTM_LOCK_READ, SDS_LOCK_TIMEOUT);
        IPropertyTree* envRoot = connEnv->queryRoot();
        if (!envRoot)
            throw MakeStringException(ECLWATCH_CANNOT_GET_ENV_INFO,"Failed to get environment information.");

        CConstWUClusterInfoArray clusters;
        getEnvironmentClusterInfo(envRoot, clusters);
        BoolHash uniqueWUIDs;

        Owned<IRemoteConnection> conn = querySDS().connect("/Status/Servers",myProcessSession(),RTM_LOCK_READ,30000);

        StringArray runningQueueNames;
        int runningJobsInQueue[256];
        for (int i = 0; i < 256; i++)
            runningJobsInQueue[i] = 0;

        IArrayOf<IEspActiveWorkunit> aws;
        if (conn.get()) 
        {
            Owned<IPropertyTreeIterator> it(conn->queryRoot()->getElements("Server[@name!=\"ECLagent\"]"));
            ForEach(*it)
                addRunningWUs(context, it->query(), clusters, aws, uniqueWUIDs, runningQueueNames, runningJobsInQueue);

            Owned<IPropertyTreeIterator> it1(conn->queryRoot()->getElements("Server[@name=\"ECLagent\"]"));
            ForEach(*it1)
                addRunningWUs(context, it1->query(), clusters, aws, uniqueWUIDs, runningQueueNames, runningJobsInQueue);
        }

        SecAccessFlags access;
        bool fullAccess=(context.authorizeFeature(THORQUEUE_FEATURE, access) && access>=SecAccess_Full);

        IArrayOf<IEspThorCluster> ThorClusters;
        IArrayOf<IEspHThorCluster> HThorClusters;
        IArrayOf<IEspRoxieCluster> RoxieClusters;

        ForEachItemIn(c, clusters)
        {
            IConstWUClusterInfo &cluster = clusters.item(c);
            SCMStringBuffer str;
            if (cluster.getThorProcesses().ordinality())
            {
                IEspThorCluster* returnCluster = new CThorCluster("","");
                returnCluster->setThorLCR(ThorLCRCluster == cluster.getPlatform() ? "withLCR" : "noLCR");
                str.clear();
                returnCluster->setClusterName(cluster.getName(str).str());
                str.clear();
                const char *queueName = cluster.getThorQueue(str).str();
                returnCluster->setQueueName(queueName);

                StringBuffer queueState;
                CJobQueueContents contents;
                Owned<IJobQueue> queue = createJobQueue(queueName);
                queue->copyItemsAndState(contents, queueState);
                addQueuedWorkUnits(queueName, contents, aws, context, "ThorMaster", NULL);

                BulletType bulletType = bulletGreen;
                int serverID = runningQueueNames.find(queueName);
                int numRunningJobsInQueue = (NotFound != serverID) ? runningJobsInQueue[serverID] : -1;
                getQueueState(numRunningJobsInQueue, queueState, bulletType);

                StringBuffer agentQueueState;
                CJobQueueContents agentContents;
                SCMStringBuffer str1;
                const char *agentQueueName = cluster.getAgentQueue(str1).str();
                Owned<IJobQueue> agentQueue = createJobQueue(agentQueueName);
                agentQueue->copyItemsAndState(agentContents, agentQueueState);
                //Use the same 'queueName' because the job belongs to the same cluster
                addQueuedWorkUnits(queueName, agentContents, aws, context, "ThorMaster", NULL);
                if (bulletType == bulletGreen)
                {//If ThorQueue is normal, check the AgentQueue
                    serverID = runningQueueNames.find(agentQueueName);
                    numRunningJobsInQueue = (NotFound != serverID) ? runningJobsInQueue[serverID] : -1;
                    getQueueState(numRunningJobsInQueue, queueState, bulletType);
                }

                returnCluster->setQueueStatus(queueState.str());
                if (version > 1.06)
                    returnCluster->setQueueStatus2(bulletType);
                if (version > 1.10)
                    returnCluster->setClusterSize(cluster.getSize());

                addToThorClusterList(ThorClusters, returnCluster, req.getSortBy(), req.getDescending());
            }
            if (version > 1.06)
            {
                str.clear();
                if (cluster.getRoxieProcess(str).length())
                {
                    IEspRoxieCluster* returnCluster = new CRoxieCluster("","");
                    str.clear();
                    returnCluster->setClusterName(cluster.getName(str).str());
                    str.clear();
                    returnCluster->setQueueName(cluster.getAgentQueue(str).str());
                    str.clear();
                    const char *queueName = cluster.getAgentQueue(str).str();
                    StringBuffer queueState;
                    CJobQueueContents contents;
                    Owned<IJobQueue> queue = createJobQueue(queueName);
                    queue->copyItemsAndState(contents, queueState);
                    addQueuedWorkUnits(queueName, contents, aws, context, "RoxieServer", NULL);

                    BulletType bulletType = bulletGreen;
                    int serverID = runningQueueNames.find(queueName);
                    int numRunningJobsInQueue = (NotFound != serverID) ? runningJobsInQueue[serverID] : -1;
                    getQueueState(numRunningJobsInQueue, queueState, bulletType);
                    returnCluster->setQueueStatus(queueState.str());
                    returnCluster->setQueueStatus2(bulletType);
                    if (version > 1.10)
                        returnCluster->setClusterSize(cluster.getSize());

                    addToRoxieClusterList(RoxieClusters, returnCluster, req.getSortBy(), req.getDescending());
                }
            }
            if (version > 1.11 && (cluster.getPlatform() == HThorCluster))
            {
                IEspHThorCluster* returnCluster = new CHThorCluster("","");
                str.clear();
                returnCluster->setClusterName(cluster.getName(str).str());
                str.clear();
                returnCluster->setQueueName(cluster.getAgentQueue(str).str());
                str.clear();
                const char *queueName = cluster.getAgentQueue(str).str();
                StringBuffer queueState;
                CJobQueueContents contents;
                Owned<IJobQueue> queue = createJobQueue(queueName);
                queue->copyItemsAndState(contents, queueState);
                addQueuedWorkUnits(queueName, contents, aws, context, "HThorServer", NULL);

                BulletType bulletType = bulletGreen;
                int serverID = runningQueueNames.find(queueName);
                int numRunningJobsInQueue = (NotFound != serverID) ? runningJobsInQueue[serverID] : -1;
                getQueueState(numRunningJobsInQueue, queueState, bulletType);
                returnCluster->setQueueStatus(queueState.str());
                returnCluster->setQueueStatus2(bulletType);
                HThorClusters.append(*returnCluster);
            }
        }
        resp.setThorClusters(ThorClusters);
        if (version > 1.06)
            resp.setRoxieClusters(RoxieClusters);
        if (version > 1.10)
        {
            resp.setSortBy(req.getSortBy());
            resp.setDescending(req.getDescending());
        }
        if (version > 1.11)
        {
            resp.setHThorClusters(HThorClusters);
            if (fullAccess)
                resp.setAccessRight("Access_Full");
        }

        IArrayOf<IEspServerJobQueue> serverJobQueues;
        IArrayOf<IConstTpEclServer> eclccservers;
        CTpWrapper dummy;
        dummy.getTpEclCCServers(envRoot->queryBranch("Software"), eclccservers);
        ForEachItemIn(x1, eclccservers)
        {
            IConstTpEclServer& eclccserver = eclccservers.item(x1);
            const char* serverName = eclccserver.getName();
            if (!serverName || !*serverName)
                continue;

            Owned <IStringIterator> targetClusters = getTargetClusters(eqEclCCServer, serverName);
            if (!targetClusters->first())
                continue;

            ForEach (*targetClusters)
            {
                SCMStringBuffer targetCluster;
                targetClusters->str(targetCluster);

                StringBuffer queueName;
                StringBuffer queueState;
                CJobQueueContents contents;
                getClusterEclCCServerQueueName(queueName, targetCluster.str());
                Owned<IJobQueue> queue = createJobQueue(queueName);
                queue->copyItemsAndState(contents, queueState);
                unsigned count=0;
                Owned<IJobQueueIterator> iter = contents.getIterator();
                ForEach(*iter) 
                {
                    if (isInWuList(aws, iter->query().queryWUID()))
                        continue;

                    Owned<IEspActiveWorkunit> wu(new CActiveWorkunitWrapper(context, iter->query().queryWUID(),++count));
                    wu->setServer("ECLCCserver");
                    wu->setInstance(serverName);
                    wu->setQueueName(queueName);

                    aws.append(*wu.getLink());
                }

                addServerJobQueue(serverJobQueues, queueName, queueState.str(), serverName, "ECLCCserver");
            }
        }

        StringBuffer dirxpath;
        dirxpath.appendf("Software/%s", eqDfu);
        Owned<IPropertyTreeIterator> services = envRoot->getElements(dirxpath);

        if (services->first())
        {
            do
            {
                IPropertyTree &serviceTree = services->query();
                const char *queuename = serviceTree.queryProp("@queue");
                const char *serverName = serviceTree.queryProp("@name");
                if (queuename && *queuename)
                {
                    StringArray queues;
                    loop
                    {
                        StringAttr subq;
                        const char *comma = strchr(queuename,',');
                        if (comma)
                            subq.set(queuename,comma-queuename);
                        else
                            subq.set(queuename);
                        bool added;
                        const char *s = strdup(subq.get());
                        queues.bAdd(s, stringcmp, added);
                        if (!added)
                            free((void *)s);
                        if (!comma)
                            break;
                        queuename = comma+1;
                        if (!*queuename)
                            break;
                    }
                    ForEachItemIn(q, queues)
                    {
                        const char *queueName = queues.item(q);

                        StringAttrArray wulist;
                        unsigned running = queuedJobs(queueName, wulist);
                        ForEachItemIn(i, wulist)
                        {
                            const char *wuid = wulist.item(i).text.get();
                            try
                            {
                                StringBuffer jname, uname, state;
                                Owned<IConstDFUWorkUnit> wu = getDFUWorkUnitFactory()->openWorkUnit(wuid, false);
                                if (wu)
                                {
                                    wu->getUser(uname);
                                    wu->getJobName(jname);
                                    if (i<running)
                                        state.append("running");
                                    else
                                        state.append("queued");

                                    Owned<IEspActiveWorkunit> wu1(new CActiveWorkunitWrapper(wuid, uname.str(), jname.str(), state.str(), "normal"));
                                    wu1->setServer("DFUserver");
                                    wu1->setInstance(serverName);
                                    wu1->setQueueName(queueName);
                                    aws.append(*wu1.getLink());
                                }
                            }
                            catch (IException *e)
                            {
                                StringBuffer msg;
                                Owned<IEspActiveWorkunit> wu1(new CActiveWorkunitWrapper(wuid, "", "", e->errorMessage(msg).str(), "normal"));
                                wu1->setServer("DFUserver");
                                wu1->setInstance(serverName);
                                wu1->setQueueName(queueName);
                                aws.append(*wu1.getLink());
                            }
                        }
                        addServerJobQueue(serverJobQueues, queueName, serverName, "DFUserver");
                    }
                }
            } while (services->next());
        }
        resp.setRunning(aws);
        if (version > 1.03)
            resp.setServerJobQueues(serverJobQueues);

        IArrayOf<IEspDFUJob> jobs;
        conn.setown(querySDS().connect("DFU/RECOVERY",myProcessSession(),0, INFINITE));
        if (conn) 
        {
            Owned<IPropertyTreeIterator> it(conn->queryRoot()->getElements("job"));
            ForEach(*it) 
            {
                IPropertyTree &e=it->query();
                if (e.getPropBool("Running",false)) 
                {
                    unsigned done;
                    unsigned total;
                    countProgress(&e,done,total);
                    Owned<IEspDFUJob> job = new CDFUJob("","");

                    job->setTimeStarted(e.queryProp("@time_started"));
                    job->setDone(done);
                    job->setTotal(total);

                    StringBuffer cmd;
                    cmd.append(e.queryProp("@command")).append(" ").append(e.queryProp("@command_parameters"));
                    job->setCommand(cmd.str());
                    jobs.append(*job.getLink());
                }
            }
        }

        resp.setDFUJobs(jobs);
    }
    catch(IException* e)
    {   
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }

    return true;
}

void CWsSMCEx::addServerJobQueue(IArrayOf<IEspServerJobQueue>& jobQueues, const char* queueName, const char* serverName, const char* serverType)
{
    if (!queueName || !*queueName || !serverName || !*serverName || !serverType || !*serverType)
        return;

    Owned<IEspServerJobQueue> jobQueue = createServerJobQueue("", "");
    jobQueue->setQueueName(queueName);
    jobQueue->setServerName(serverName);
    jobQueue->setServerType(serverType);

    Owned<IJobQueue> queue = createJobQueue(queueName);
    if (queue->stopped())
        jobQueue->setQueueStatus("stopped");
    else if (queue->paused())
        jobQueue->setQueueStatus("paused");
    else
        jobQueue->setQueueStatus("running");

    jobQueues.append(*jobQueue.getClear());
}

void CWsSMCEx::addServerJobQueue(IArrayOf<IEspServerJobQueue>& jobQueues, const char* queueName, const char* queueState, const char* serverName, const char* serverType)
{
    if (!queueName || !*queueName || !serverName || !*serverName || !serverType || !*serverType)
        return;

    Owned<IEspServerJobQueue> jobQueue = createServerJobQueue("", "");
    jobQueue->setQueueName(queueName);
    jobQueue->setServerName(serverName);
    jobQueue->setServerType(serverType);

    if (queueState && (strieq(queueState,"stopped") || strieq(queueState,"paused")))
        jobQueue->setQueueStatus(queueState);
    else
        jobQueue->setQueueStatus("running");

    jobQueues.append(*jobQueue.getClear());
}

void CWsSMCEx::addToThorClusterList(IArrayOf<IEspThorCluster>& clusters, IEspThorCluster* cluster, const char* sortBy, bool descending)
{
    if (clusters.length() < 1)
    {
        clusters.append(*cluster);
        return;
    }

    const char* clusterName = cluster->getClusterName();
    unsigned clusterSize = cluster->getClusterSize();
    bool clusterAdded = false;
    ForEachItemIn(i, clusters)
    {
        int strCmp = 0;
        IEspThorCluster& cluster1 = clusters.item(i);
        if (!sortBy || !*sortBy || strieq(sortBy, "name"))
        {
            strCmp = strcmp(cluster1.getClusterName(), clusterName);
        }
        else
        {//size
            //strCmp = cluster1.getClusterSize() - clusterSize;
            int si = cluster1.getClusterSize();
            strCmp = si - clusterSize;
        }

        if ((descending && (strCmp < 0)) || (!descending && (strCmp > 0)))
        {
            clusters.add(*cluster, i);
            clusterAdded =  true;
            break;
        }
    }

    if (!clusterAdded)
        clusters.append(*cluster);
    return;
}

void CWsSMCEx::addToRoxieClusterList(IArrayOf<IEspRoxieCluster>& clusters, IEspRoxieCluster* cluster, const char* sortBy, bool descending)
{
    if (clusters.length() < 1)
    {
        clusters.append(*cluster);
        return;
    }

    const char* clusterName = cluster->getClusterName();
    unsigned clusterSize = cluster->getClusterSize();
    bool clusterAdded = false;
    ForEachItemIn(i, clusters)
    {
        int strCmp = 0;
        IEspRoxieCluster& cluster1 = clusters.item(i);
        if (!sortBy || !*sortBy || strieq(sortBy, "name"))
        {
            strCmp = strcmp(cluster1.getClusterName(), clusterName);
        }
        else
        {//size
            strCmp = cluster1.getClusterSize() - clusterSize;
        }

        if ((descending && (strCmp < 0)) || (!descending && (strCmp > 0)))
        {
            clusters.add(*cluster, i);
            clusterAdded = true;
            break;
        }
    }

    if (!clusterAdded)
        clusters.append(*cluster);
    return;
}


void CWsSMCEx::addCapabilities(IPropertyTree* pFeatureNode, const char* access, 
                                         IArrayOf<IEspCapability>& capabilities)
{
    StringBuffer xpath(access);
    xpath.append("/Capability");

    Owned<IPropertyTreeIterator> it = pFeatureNode->getElements(xpath.str());
    ForEach(*it)
    {
        IPropertyTree* pCapabilityNode = &it->query();

        IEspCapability* pCapability = new CCapability("ws_smc");
        pCapability->setName( pCapabilityNode->queryProp("@name") );
        pCapability->setDescription( pCapabilityNode->queryProp("@description") );

        capabilities.append(*pCapability);
    }
}

static void checkAccess(IEspContext &context, const char* feature,int level)
{
    if (!context.validateFeatureAccess(feature, level, false))
        throw MakeStringException(ECLWATCH_THOR_QUEUE_ACCESS_DENIED, "Failed to access the queue functions. Permission denied.");
}


bool CWsSMCEx::onMoveJobDown(IEspContext &context, IEspSMCJobRequest &req, IEspSMCJobResponse &resp)
{
    try
    {
        checkAccess(context,THORQUEUE_FEATURE,SecAccess_Full);

        Owned<IJobQueue> queue = createJobQueue(req.getQueueName());
        QueueLock lock(queue);
        unsigned index=queue->findRank(req.getWuid());
        if(index<queue->ordinality())
        {
            IJobQueueItem * item0 = queue->getItem(index);
            IJobQueueItem * item = queue->getItem(index+1);
            if(item && item0 && (item0->getPriority() == item->getPriority()))
                queue->moveAfter(req.getWuid(),item->queryWUID());
        }

        AccessSuccess(context, "Changed job priority %s",req.getWuid());
        resp.setRedirectUrl("/WsSMC/");
    }
    catch(IException* e)
    {   
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsSMCEx::onMoveJobUp(IEspContext &context, IEspSMCJobRequest &req, IEspSMCJobResponse &resp)
{
    try
    {
        checkAccess(context,THORQUEUE_FEATURE,SecAccess_Full);

        Owned<IJobQueue> queue = createJobQueue(req.getQueueName());
        QueueLock lock(queue);
        unsigned index=queue->findRank(req.getWuid());
        if(index>0 && index<queue->ordinality())
        {
            IJobQueueItem * item0 = queue->getItem(index);
            IJobQueueItem * item = queue->getItem(index-1);
            if(item && item0 && (item0->getPriority() == item->getPriority()))
                queue->moveBefore(req.getWuid(),item->queryWUID());
        }

        AccessSuccess(context, "Changed job priority %s",req.getWuid());
        resp.setRedirectUrl("/WsSMC/");
    }
    catch(IException* e)
    {   
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsSMCEx::onMoveJobBack(IEspContext &context, IEspSMCJobRequest &req, IEspSMCJobResponse &resp)
{
    try
    {
        checkAccess(context,THORQUEUE_FEATURE,SecAccess_Full);

        Owned<IJobQueue> queue = createJobQueue(req.getQueueName());
        QueueLock lock(queue);
        
        unsigned index=queue->findRank(req.getWuid());
        if(index<queue->ordinality())
        {
            int priority0 = queue->getItem(index)->getPriority();
            unsigned biggestIndoxInSamePriority = index;
            unsigned nextIndex = biggestIndoxInSamePriority + 1;
            while (nextIndex<queue->ordinality())
            {
                IJobQueueItem * item = queue->getItem(nextIndex);
                if (priority0 != item->getPriority())
                {
                    break;
                }
                biggestIndoxInSamePriority = nextIndex;
                nextIndex++;
            }

            if (biggestIndoxInSamePriority != index)
            {
                IJobQueueItem * item = queue->getItem(biggestIndoxInSamePriority);
                queue->moveAfter(req.getWuid(),item->queryWUID());
            }
        }

        AccessSuccess(context, "Changed job priority %s",req.getWuid());
        resp.setRedirectUrl("/WsSMC/");
    }
    catch(IException* e)
    {   
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsSMCEx::onMoveJobFront(IEspContext &context, IEspSMCJobRequest &req, IEspSMCJobResponse &resp)
{
    try
    {
        checkAccess(context,THORQUEUE_FEATURE,SecAccess_Full);

        Owned<IJobQueue> queue = createJobQueue(req.getQueueName());
        QueueLock lock(queue);
        
        unsigned index=queue->findRank(req.getWuid());
        if(index>0 && index<queue->ordinality())
        {
            int priority0 = queue->getItem(index)->getPriority();
            unsigned smallestIndoxInSamePriority = index;
            int nextIndex = smallestIndoxInSamePriority - 1;
            while (nextIndex >= 0)
            {
                IJobQueueItem * item = queue->getItem(nextIndex);
                if (priority0 != item->getPriority())
                {
                    break;
                }
                smallestIndoxInSamePriority = nextIndex;
                nextIndex--;
            }

            if (smallestIndoxInSamePriority != index)
            {
                IJobQueueItem * item = queue->getItem(smallestIndoxInSamePriority);
                queue->moveBefore(req.getWuid(),item->queryWUID());
            }
        }

        AccessSuccess(context, "Changed job priority %s",req.getWuid());
        resp.setRedirectUrl("/WsSMC/");
    }
    catch(IException* e)
    {   
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsSMCEx::onRemoveJob(IEspContext &context, IEspSMCJobRequest &req, IEspSMCJobResponse &resp)
{
    try
    {
        checkAccess(context,THORQUEUE_FEATURE,SecAccess_Full);

        secAbortWorkUnit(req.getWuid(), *context.querySecManager(), *context.queryUser());

        Owned<IJobQueue> queue = createJobQueue(req.getQueueName());
        QueueLock lock(queue);
        
        unsigned index=queue->findRank(req.getWuid());
        if(index<queue->ordinality())
        {
            if(!queue->cancelInitiateConversation(req.getWuid()))
                throw MakeStringException(ECLWATCH_CANNOT_DELETE_WORKUNIT,"Failed to remove the workunit %s",req.getWuid());
        }

        AccessSuccess(context, "Removed job %s",req.getWuid());
        resp.setRedirectUrl("/WsSMC/");
    }
    catch(IException* e)
    {   
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsSMCEx::onStopQueue(IEspContext &context, IEspSMCQueueRequest &req, IEspSMCQueueResponse &resp)
{
    try
    {
        checkAccess(context,THORQUEUE_FEATURE,SecAccess_Full);

        Owned<IJobQueue> queue = createJobQueue(req.getQueueName());
        queue->stop();
        AccessSuccess(context, "Stopped queue %s",req.getCluster());
        resp.setRedirectUrl("/WsSMC/");
    }
    catch(IException* e)
    {   
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsSMCEx::onResumeQueue(IEspContext &context, IEspSMCQueueRequest &req, IEspSMCQueueResponse &resp)
{
    try
    {
        checkAccess(context,THORQUEUE_FEATURE,SecAccess_Full);

        Owned<IJobQueue> queue = createJobQueue(req.getQueueName());
        queue->resume();
        AccessSuccess(context, "Resumed queue %s",req.getCluster());
        resp.setRedirectUrl("/WsSMC/");
    }
    catch(IException* e)
    {   
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsSMCEx::onPauseQueue(IEspContext &context, IEspSMCQueueRequest &req, IEspSMCQueueResponse &resp)
{
    try
    {
        checkAccess(context,THORQUEUE_FEATURE,SecAccess_Full);

        Owned<IJobQueue> queue = createJobQueue(req.getQueueName());
        queue->pause();
        AccessSuccess(context, "Paused queue %s",req.getCluster());
        resp.setRedirectUrl("/WsSMC/");
    }
    catch(IException* e)
    {   
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsSMCEx::onClearQueue(IEspContext &context, IEspSMCQueueRequest &req, IEspSMCQueueResponse &resp)
{
    try
    {
        checkAccess(context,THORQUEUE_FEATURE,SecAccess_Full);
        Owned<IJobQueue> queue = createJobQueue(req.getQueueName());
        {
            QueueLock lock(queue);
            for(unsigned i=0;i<queue->ordinality();i++)
                secAbortWorkUnit(queue->getItem(i)->queryWUID(), *context.querySecManager(), *context.queryUser());
            queue->clear();
        }
        AccessSuccess(context, "Cleared queue %s",req.getCluster());
        resp.setRedirectUrl("/WsSMC/");
    }
    catch(IException* e)
    {   
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsSMCEx::onSetJobPriority(IEspContext &context, IEspSMCPriorityRequest &req, IEspSMCPriorityResponse &resp)
{
    try
    {
        Owned<IWorkUnitFactory> factory = getSecWorkUnitFactory(*context.querySecManager(), *context.queryUser());
        Owned<IWorkUnit> lw = factory->updateWorkUnit(req.getWuid());

        if(stricmp(req.getPriority(),"high")==0)
            lw->setPriority(PriorityClassHigh);
        else if(stricmp(req.getPriority(),"normal")==0)
            lw->setPriority(PriorityClassNormal);
        else if(stricmp(req.getPriority(),"low")==0)
            lw->setPriority(PriorityClassLow);

        // set job priority
        int priority = lw->getPriorityValue();  
        Owned<IJobQueue> queue = createJobQueue(req.getQueueName());
        QueueLock lock(queue);
        queue->changePriority(req.getWuid(),priority);

        resp.setRedirectUrl("/WsSMC/");
    }
    catch(IException* e)
    {   
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsSMCEx::onGetThorQueueAvailability(IEspContext &context, IEspGetThorQueueAvailabilityRequest &req, IEspGetThorQueueAvailabilityResponse& resp)
{
    try
    {
        if (!context.validateFeatureAccess(FEATURE_URL, SecAccess_Read, false))
            throw MakeStringException(ECLWATCH_SMC_ACCESS_DENIED, "Failed to get Thor Queue availability. Permission denied.");

        StringArray thorNames, groupNames, targetNames, queueNames;
        getEnvironmentThorClusterNames(thorNames, groupNames, targetNames, queueNames);

        IArrayOf<IEspThorCluster> ThorClusters;
        ForEachItemIn(x, thorNames)
        {
            const char* targetName = targetNames.item(x);
            const char* queueName = queueNames.item(x);
            IEspThorCluster* returnCluster = new CThorCluster("","");
                
            returnCluster->setClusterName(targetName);
            returnCluster->setQueueName(queueName);

            Owned<IJobQueue> queue = createJobQueue(queueName);
            if(queue->stopped())
                returnCluster->setQueueStatus("stopped");
            else if (queue->paused())
                returnCluster->setQueueStatus("paused");
            else
                returnCluster->setQueueStatus("running");

            unsigned enqueued=0;
            unsigned connected=0;
            unsigned waiting=0;
            queue->getStats(connected,waiting,enqueued);
            returnCluster->setQueueAvailable(waiting);
            returnCluster->setJobsRunning(connected - waiting);
            returnCluster->setJobsInQueue(enqueued);

            ThorClusters.append(*returnCluster);
        }

        resp.setThorClusters(ThorClusters);
    }
    catch(IException* e)
    {   
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsSMCEx::onSetBanner(IEspContext &context, IEspSetBannerRequest &req, IEspSetBannerResponse& resp)
{
    try
    {
#ifdef _USE_OPENLDAP
        CLdapSecManager* secmgr = dynamic_cast<CLdapSecManager*>(context.querySecManager());
        if(!secmgr || !secmgr->isSuperUser(context.queryUser()))
            throw MakeStringException(ECLWATCH_SUPER_USER_ACCESS_DENIED, "access denied, administrators only.");
#endif
        StringBuffer chatURLStr, bannerStr;
        const char* chatURL = req.getChatURL();
        const char* banner = req.getBannerContent();
        //Only display valid strings
        if (chatURL)
        {
            const char* pStr = chatURL;
            for (unsigned i = 0; i < strlen(chatURL); i++)
            {
                if ((pStr[0] > 31) && (pStr[0] < 127))
                    chatURLStr.append(pStr[0]);
                pStr++;
            }
        }
        if (banner)
        {
            const char* pStr = banner;
            for (unsigned i = 0; i < strlen(banner); i++)
            {
                if ((pStr[0] > 31) && (pStr[0] < 127))
                    bannerStr.append(pStr[0]);
                pStr++;
            }
        }
        chatURLStr.trim();
        bannerStr.trim();

        if (!req.getBannerAction_isNull() && req.getBannerAction() && (bannerStr.length() < 1))
        {
            throw MakeStringException(ECLWATCH_MISSING_BANNER_CONTENT, "If a Banner is enabled, the Banner content must be specified.");
        }

        if (!req.getEnableChatURL_isNull() && req.getEnableChatURL() && (!req.getChatURL() || !*req.getChatURL()))
        {
            throw MakeStringException(ECLWATCH_MISSING_CHAT_URL, "If a Chat is enabled, the Chat URL must be specified.");
        }

        m_ChatURL = chatURLStr;
        m_Banner = bannerStr;

        const char* bannerSize = req.getBannerSize();
        if (bannerSize && *bannerSize)
            m_BannerSize.clear().append(bannerSize);

        const char* bannerColor = req.getBannerColor();
        if (bannerColor && *bannerColor)
            m_BannerColor.clear().append(bannerColor);

        const char* bannerScroll = req.getBannerScroll();
        if (bannerScroll && *bannerScroll)
            m_BannerScroll.clear().append(bannerScroll);

        m_BannerAction = 0;
        if(!req.getBannerAction_isNull())
            m_BannerAction = req.getBannerAction();

        m_EnableChatURL = 0;
        if(!req.getEnableChatURL_isNull())
            m_EnableChatURL = req.getEnableChatURL();

        resp.setRedirectUrl("/WsSMC/Activity");
    }
    catch(IException* e)
    {   
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return true;
}

bool CWsSMCEx::onNotInCommunityEdition(IEspContext &context, IEspNotInCommunityEditionRequest &req, IEspNotInCommunityEditionResponse &resp)
{
   return true;
}

bool CWsSMCEx::onBrowseResources(IEspContext &context, IEspBrowseResourcesRequest & req, IEspBrowseResourcesResponse & resp)
{
    try
    {
        if (!context.validateFeatureAccess(FEATURE_URL, SecAccess_Read, false))
            throw MakeStringException(ECLWATCH_SMC_ACCESS_DENIED, "Failed to Browse Resources. Permission denied.");

        double version = context.getClientVersion();

        Owned<IEnvironmentFactory> factory = getEnvironmentFactory();
        Owned<IConstEnvironment> constEnv = factory->openEnvironmentByFile();

        //The resource files will be downloaded from the same box of ESP (not dali)
        StringBuffer ipStr;
        IpAddress ipaddr = queryHostIP();
        ipaddr.getIpText(ipStr);
        if (ipStr.length() > 0)
        {
            resp.setNetAddress(ipStr.str());
            Owned<IConstMachineInfo> machine = constEnv->getMachineByAddress(ipStr.str());
            if (machine)
            {
                int os = machine->getOS();
                resp.setOS(os);
            }
        }

        if (m_PortalURL.length() > 0)
            resp.setPortalURL(m_PortalURL.str());

#ifndef USE_RESOURCE
        if (version > 1.12)
            resp.setUseResource(false);
#else
        if (version > 1.12)
            resp.setUseResource(true);

        //Now, get a list of resources stored inside the ESP box
        IArrayOf<IEspHPCCResourceRepository> resourceRepositories;

        Owned<IPropertyTree> pEnvRoot = &constEnv->getPTree();
        const char* ossInstall = pEnvRoot->queryProp("EnvSettings/path");
        if (!ossInstall || !*ossInstall)
        {
            WARNLOG("Failed to get EnvSettings/Path in environment settings.");
            return true;
        }

        StringBuffer path;
        path.appendf("%s/componentfiles/files/downloads", ossInstall);
        Owned<IFile> f = createIFile(path.str());
        if(!f->exists() || !f->isDirectory())
        {
            WARNLOG("Invalid resource folder");
            return true;
        }

        Owned<IDirectoryIterator> di = f->directoryFiles(NULL, false, true);
        if(di.get() == NULL)
        {
            WARNLOG("Resource folder is empty.");
            return true;
        }

        ForEach(*di)
        {
            if (!di->isDir())
                continue;

            StringBuffer folder, path0, tmpBuf;
            di->getName(folder);
            if (folder.length() == 0)
                continue;

            path0.appendf("%s/%s/description.xml", path.str(), folder.str());
            Owned<IFile> f0 = createIFile(path0.str());
            if(!f0->exists())
            {
                WARNLOG("Description file not found for %s", folder.str());
                continue;
            }

            OwnedIFileIO rIO = f0->openShared(IFOread,IFSHfull);
            if(!rIO)
            {
                WARNLOG("Failed to open the description file for %s", folder.str());
                continue;
            }

            offset_t fileSize = f0->size();
            tmpBuf.ensureCapacity((unsigned)fileSize);
            tmpBuf.setLength((unsigned)fileSize);

            size32_t nRead = rIO->read(0, (size32_t) fileSize, (char*)tmpBuf.str());
            if (nRead != fileSize)
            {
                WARNLOG("Failed to read the description file for %s", folder.str());
                continue;
            }

            Owned<IPropertyTree> desc = createPTreeFromXMLString(tmpBuf.str());
            if (!desc)
            {
                WARNLOG("Invalid description file for %s", folder.str());
                continue;
            }

            Owned<IPropertyTreeIterator> fileIterator = desc->getElements("file");
            if (!fileIterator->first())
            {
                WARNLOG("Invalid description file for %s", folder.str());
                continue;
            }

            IArrayOf<IEspHPCCResource> resourcs;

            do {
                IPropertyTree &fileItem = fileIterator->query();
                const char* filename = fileItem.queryProp("filename");
                if (!filename || !*filename)
                    continue;

                const char* name0 = fileItem.queryProp("name");
                const char* description0 = fileItem.queryProp("description");
                const char* version0 = fileItem.queryProp("version");

                Owned<IEspHPCCResource> onefile = createHPCCResource();
                onefile->setFileName(filename);
                if (name0 && *name0)
                    onefile->setName(name0);
                if (description0 && *description0)
                    onefile->setDescription(description0);
                if (version0 && *version0)
                    onefile->setVersion(version0);

                resourcs.append(*onefile.getLink());
            } while (fileIterator->next());

            if (resourcs.ordinality())
            {
                StringBuffer path1;
                path1.appendf("%s/%s", path.str(), folder.str());

                Owned<IEspHPCCResourceRepository> oneRepository = createHPCCResourceRepository();
                oneRepository->setName(folder.str());
                oneRepository->setPath(path1.str());
                oneRepository->setHPCCResources(resourcs);

                resourceRepositories.append(*oneRepository.getLink());
            }
        }

        if (resourceRepositories.ordinality())
            resp.setHPCCResourceRepositories(resourceRepositories);
#endif
    }
    catch(IException* e)
    {
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }

    return true;
}

int CWsSMCSoapBindingEx::onGetForm(IEspContext &context, CHttpRequest* request, CHttpResponse* response, const char *service, const char *method)
{
    try
    {
        if(stricmp(method,"NotInCommunityEdition")==0)
        {
            StringBuffer page, url, link;
            request->getParameter("EEPortal", url);
            if (url.length() > 0)
                link.appendf("Further information can be found at <a href=\"%s\" target=\"_blank\">%s</a>.", url.str(), url.str());

            page.append(
                "<html>"
                    "<head>"
                        "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
                        "<link rel=\"stylesheet\" type=\"text/css\" href=\"/esp/files/default.css\"/>"
                        "<link rel=\"stylesheet\" type=\"text/css\" href=\"/esp/files/yui/build/fonts/fonts-min.css\" />"
                        "<title>Advanced feature in Enterprise Edition</title>"
                    "</head>"
                    "<body>"
                        "<h3 style=\"text-align:centre;\">Advanced feature in the Enterprise Edition</h4>"
                        "<p style=\"text-align:centre;\">Support for this feature is coming soon. ");
            if (link.length() > 0)
                page.append(link.str());
            page.append("</p></body>"
                "</html>");

            response->setContent(page.str());
            response->setContentType("text/html");
            response->send();
            return 0;
        }
        else if(stricmp(method,"DisabledInThisVersion")==0)
        {
            StringBuffer page;
            page.append(
                "<html>"
                    "<head>"
                        "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
                        "<link rel=\"stylesheet\" type=\"text/css\" href=\"/esp/files/default.css\"/>"
                        "<link rel=\"stylesheet\" type=\"text/css\" href=\"/esp/files/yui/build/fonts/fonts-min.css\" />"
                        "<title>Disabled Feature in This Version</title>"
                    "</head>"
                    "<body>"
                        "<h3 style=\"text-align:centre;\">Disabled Feature in This Version</h4>"
                        "<p style=\"text-align:centre;\">This feature is disabled in this version. ");
            page.append("</p></body>"
                "</html>");

            response->setContent(page.str());
            response->setContentType("text/html");
            response->send();
            return 0;
        }
    }
    catch(IException* e)
    {   
        FORWARDEXCEPTION(context, e,  ECLWATCH_INTERNAL_ERROR);
    }
    return onGetForm(context, request, response, service, method);
}

inline const char *controlCmdMessage(int cmd)
{
    switch (cmd)
    {
    case CRoxieControlCmd_ATTACH:
        return "<control:unlockDali/>";
    case CRoxieControlCmd_DETACH:
        return "<control:lockDali/>";
    case CRoxieControlCmd_RELOAD:
        return "<control:reload/>";
    case CRoxieControlCmd_STATE:
        return "<control:state/>";
    default:
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "Unknown Roxie Control Command.");
    }
    return NULL;
}

bool CWsSMCEx::onRoxieControlCmd(IEspContext &context, IEspRoxieControlCmdRequest &req, IEspRoxieControlCmdResponse &resp)
{
    if (!context.validateFeatureAccess(ROXIE_CONTROL_URL, SecAccess_Full, false))
       throw MakeStringException(ECLWATCH_SMC_ACCESS_DENIED, "Cannot Access Roxie Control. Permission denied.");

    const char *process = req.getProcessCluster();
    if (!process || !*process)
        throw MakeStringException(ECLWATCH_MISSING_PARAMS, "Process cluster not specified.");
    const char *controlReq = controlCmdMessage(req.getCommand());

    SocketEndpointArray addrs;
    getRoxieProcessServers(process, addrs);
    if (!addrs.length())
        throw MakeStringException(ECLWATCH_CANNOT_GET_ENV_INFO, "Process cluster not found.");
    Owned<IPropertyTree> controlResp = sendRoxieControlAllNodes(addrs.item(0), controlReq, true, req.getWait());
    if (!controlResp)
        throw MakeStringException(ECLWATCH_INTERNAL_ERROR, "Failed to get control response from roxie.");

    IArrayOf<IEspRoxieControlEndpointInfo> respEndpoints;
    Owned<IPropertyTreeIterator> roxieEndpoints = controlResp->getElements("Endpoint");
    ForEach(*roxieEndpoints)
    {
        IPropertyTree &roxieEndpoint = roxieEndpoints->query();
        Owned<IEspRoxieControlEndpointInfo> respEndpoint = createRoxieControlEndpointInfo();
        respEndpoint->setAddress(roxieEndpoint.queryProp("@ep"));
        respEndpoint->setStatus(roxieEndpoint.queryProp("Status"));
        if (roxieEndpoint.hasProp("Dali/@connected"))
            respEndpoint->setAttached(roxieEndpoint.getPropBool("Dali/@connected"));
        if (roxieEndpoint.hasProp("State/@hash"))
            respEndpoint->setStateHash(roxieEndpoint.queryProp("State/@hash"));
        respEndpoints.append(*respEndpoint.getClear());
    }
    resp.setEndpoints(respEndpoints);
    return true;
}
