
#include "cms/cms_errorcode.h"
#include "cms/cmu/cmu_proto.h"

#include "devlogintask.h"

CGeneralLoginList g_cGeneralLoginList;

CLoginSenderTask::CLoginSenderTask(CInstExt *pcInst) : CLoginTask(pcInst)
{
}

CLoginSenderTask::~CLoginSenderTask()
{
    ReleaseResource();
}

void CLoginSenderTask::ReleaseResource()
{
    TASKLOG(EVENT_LEV, "dev[%s-%s]-sess[%s]登录事务被销毁\n",
        GetDevType().c_str(), GetDevUri().c_str(), GetSession().c_str());

    switch(GetState())
    {
    case WaitLogin:
        {
        }
        break;

    case Service:
        {
            //清除该登录会话下所有事务
            DestroyChildTasks();

            //停止心跳
            StopHB(GetServerSipUri());

            //更新设备登录列表
            g_cGeneralLoginList.Erase(GetSession());
        }
        break;

    case WaitLogout:
        {
            //清除该登录会话下所有事务
            DestroyChildTasks();

            //更新设备登录列表
            g_cGeneralLoginList.Erase(GetSession());
        }
        break;

    default:
        {
            TASKLOG(ERROR_LEV, "未知事务状态[%d]\n", GetState());
        }
        break;
    }
}

void CLoginSenderTask::InitStateMachine()
{
    CStateProc cWaitLoginProc;
    cWaitLoginProc.ProcMsg = (CTask::PFProcMsg)&CLoginSenderTask::OnWaitLogin;
    cWaitLoginProc.ProcErrMsg = (CTask::PFProcMsg)&CLoginSenderTask::OnWaitLogin;
    cWaitLoginProc.TimerPoll = (CTask::PFTimerPoll)&CLoginSenderTask::OnWaitLoginTimer;
    AddRuleProc(WaitLogin, cWaitLoginProc);

    CStateProc cServiceProc;
    cServiceProc.ProcMsg = (CTask::PFProcMsg)&CLoginSenderTask::OnService;
    cServiceProc.ProcErrMsg = (CTask::PFProcMsg)&CLoginSenderTask::OnService;
    cServiceProc.TimerPoll = (CTask::PFTimerPoll)&CLoginSenderTask::OnServiceTimer;
    AddRuleProc(Service, cServiceProc);

    CStateProc cWaitLogoutProc;
    cWaitLogoutProc.ProcMsg = (CTask::PFProcMsg)&CLoginSenderTask::OnWaitLogout;
    cWaitLogoutProc.ProcErrMsg = (CTask::PFProcMsg)&CLoginSenderTask::OnWaitLogout;
    cWaitLogoutProc.TimerPoll = (CTask::PFTimerPoll)&CLoginSenderTask::OnWaitLogoutTimer;
    AddRuleProc(WaitLogout, cWaitLogoutProc);

    NextState(WaitLogin);
}

void CLoginSenderTask::StartLogin()
{
    NextState(WaitLogin);

    CDevLoginReq cReq;
    cReq.SetDevUri(GetDevUri());
    cReq.SetDevType(GetDevType());
    cReq.SetDevAddrList(GetDevAddrList());

    u32 dwRet = PROCMSG_FAIL;
    dwRet = PostMsgReq(cReq, GetServerSipUri());
    if (dwRet != PROCMSG_OK)
    {
        TASKLOG(ERROR_LEV, "发送SIP消息失败!\n");
    }
}

void CLoginSenderTask::StartLogout()
{
    if (!IsService())
    {
        TASKLOG(ERROR_LEV, "还没有登录成功，无需登出!\n");
        return;
    }

    CDevLogoutReq cReq;
    cReq.SetSession(GetSession());
    cReq.SetDevUri(GetDevUri());

    u32 dwRet = PROCMSG_FAIL;
    dwRet = PostMsgReq(cReq, GetServerSipUri());
    if (dwRet != PROCMSG_OK)
    {
        TASKLOG(ERROR_LEV, "发送SIP消息失败!\n");
    }

    StopHB(GetServerSipUri());

    NextState(WaitLogout);
}

u32 CLoginSenderTask::OnWaitLogin(CMessage *const pcMsg)
{
    u32 dwRet = PROCMSG_FAIL;
    switch(pcMsg->event)
    {
    case DEV_LOGIN_RSP:
        {
            //设备收到登录应答
            COspSipMsg* pcOspSipMsg = (COspSipMsg*)pcMsg->content;
            //不再检查pcOspSipMsg是否为空，如果为空就直接崩溃吧

            CDevLoginRsp cRsp;
            pcOspSipMsg->GetMsgBody(cRsp);
            u32 dwErrorCode = cRsp.GetErrorCode();
            if (dwErrorCode == CMS_SUCCESS)
            {
                //登录成功
                TASKLOG(CRITICAL_LEV, "登录服务器[%s]成功!\n",
                    GetServerUri().c_str());

                //保存session号
                SetSession(cRsp.GetSession());

                //开始心跳
                SetHBParam(GetServerSipUri());

                //进入服务态
                NextState(Service);

                dwRet = PROCMSG_OK;
            }
            else
            {
                //登录失败
                TASKLOG(ERROR_LEV, "登录服务器[%s]失败,等待定时器重连，errcode[%d]\n",
                    GetServerUri().c_str(), dwErrorCode);

                dwRet = PROCMSG_OK;
            }
        }
        break;

    case OSP_SIP_MSG_PROC_FAIL:
        {
            //设备发起的登录请求收到SIP层的错误应答
            COspSipMsg* pcOspSipMsg = (COspSipMsg*)pcMsg->content;
            //不再检查pcOspSipMsg是否为空，如果为空就直接崩溃吧

            //登录失败
            TASKLOG(ERROR_LEV, "登录服务器[%s]失败,等待定时器重连，sip status code[%d]\n",
                GetServerUri().c_str(), pcOspSipMsg->GetSipErrorCode());

            dwRet = PROCMSG_OK;
        }
        break;

    default:
        {
            TASKLOG(WARNING_LEV, "Receive unkown Msg[%s-%d]\n", OspExtEventDesc(pcMsg->event).c_str(), pcMsg->event);
        }
        break;
    }

    return dwRet;
}

u32 CLoginSenderTask::OnWaitLoginTimer()
{
    if (GetCurStateHoldTime() > DEV_LOGIN_TIMEOUT)
    {
        TASKLOG(WARNING_LEV, "登录服务器[%s]超时,等待定时器重连\n",
            GetServerUri().c_str());

        //重新登录
        StartLogin();
    }

    return TIMERPOLL_DONE;
}

u32 CLoginSenderTask::OnService(CMessage *const pcMsg)
{
    u32 dwRet = PROCMSG_FAIL;
    switch(pcMsg->event)
    {
    case OSP_SIP_DISCONNECT:
        {
            TASKLOG(CRITICAL_LEV, "dev[%s]和server[%s]断链, 等待定时器重连\n", 
                GetDevUri().c_str(), GetServerUri().c_str());

            //重新登录
            StartLogin();

            dwRet = PROCMSG_OK;
        }
        break;

    default:
        {
            TASKLOG(WARNING_LEV, "Receive unkown Msg[%s-%d]\n", OspExtEventDesc(pcMsg->event).c_str(), pcMsg->event);
        }
        break;
    }

    return dwRet;
}

u32 CLoginSenderTask::OnServiceTimer()
{
    return TIMERPOLL_DONE;
}

u32 CLoginSenderTask::OnWaitLogout(CMessage *const pcMsg)
{
    u32 dwRet = PROCMSG_FAIL;
    switch(pcMsg->event)
    {
    case DEV_LOGOUT_RSP:
        {
            //设备收到登出应答
            COspSipMsg* pcOspSipMsg = (COspSipMsg*)pcMsg->content;
            //不再检查pcOspSipMsg是否为空，如果为空就直接崩溃吧

            CDevLogoutRsp cRsp;
            pcOspSipMsg->GetMsgBody(cRsp);
            u32 dwErrorCode = cRsp.GetErrorCode();
            if (dwErrorCode == CMS_SUCCESS)
            {
                //登出成功
                TASKLOG(CRITICAL_LEV, "登出服务器[%s]成功!\n",
                    GetServerUri().c_str());         
            }
            else
            {
                //登出失败
                TASKLOG(ERROR_LEV, "登出服务器[%s]失败，errcode[%d]，需要强退\n",
                    GetServerUri().c_str(), dwErrorCode);
            }

            //不管怎样，都要删除
            dwRet = PROCMSG_DEL;
        }
        break;

    case OSP_SIP_MSG_PROC_FAIL:
        {
            //设备发起的登录请求收到SIP层的错误应答
            COspSipMsg* pcOspSipMsg = (COspSipMsg*)pcMsg->content;
            //不再检查pcOspSipMsg是否为空，如果为空就直接崩溃吧

            //登出失败
            TASKLOG(ERROR_LEV, "登出服务器[%s]失败, sip status code[%d]，需要强退\n",
                GetServerUri().c_str(), pcOspSipMsg->GetSipErrorCode());

            //不管怎样，都要删除
            dwRet = PROCMSG_DEL;
        }
        break;

    default:
        {
            TASKLOG(WARNING_LEV, "Receive unkown Msg[%s-%d]\n", OspExtEventDesc(pcMsg->event).c_str(), pcMsg->event);
        }
        break;
    }

    return dwRet;
}

u32 CLoginSenderTask::OnWaitLogoutTimer()
{
    if (GetCurStateHoldTime() > DEV_LOGIN_TIMEOUT)
    {
        TASKLOG(WARNING_LEV, "登出服务器[%s]超时,需要强退\n",
            GetServerUri().c_str());

        return TIMERPOLL_DEL;
    }

    return TIMERPOLL_DONE;
}

//////////////////////////////////////////////////////////////////////////////////////////////

CLoginRecverTask::CLoginRecverTask(CInstExt *pcInst) : CLoginTask(pcInst)
{
}

CLoginRecverTask::~CLoginRecverTask()
{
    ReleaseResource();
}

void CLoginRecverTask::ReleaseResource()
{
    TASKLOG(EVENT_LEV, "dev[%s-%s]-sess[%s]登录事务被销毁\n",
        GetDevType().c_str(), GetDevUri().c_str(), GetSession().c_str());

    switch(GetState())
    {
    case WaitLogin:
        {
        }
        break;

    case Service:
        {
            //清除该登录会话下所有事务
            DestroyChildTasks();

            //更新设备登录列表
            g_cGeneralLoginList.Erase(GetSession());
        }
        break;

    default:
        {
            TASKLOG(ERROR_LEV, "未知事务状态[%d]\n", GetState());
        }
        break;
    }
}

void CLoginRecverTask::InitStateMachine()
{
    CStateProc cWaitLoginProc;
    cWaitLoginProc.ProcMsg = (CTask::PFProcMsg)&CLoginRecverTask::OnWaitLogin;
    cWaitLoginProc.ProcErrMsg = (CTask::PFProcMsg)&CLoginRecverTask::OnWaitLogin;
    cWaitLoginProc.TimerPoll = (CTask::PFTimerPoll)&CLoginRecverTask::OnWaitLoginTimer;
    AddRuleProc(WaitLogin, cWaitLoginProc);

    CStateProc cServiceProc;
    cServiceProc.ProcMsg = (CTask::PFProcMsg)&CLoginRecverTask::OnService;
    cServiceProc.ProcErrMsg = (CTask::PFProcMsg)&CLoginRecverTask::OnService;
    cServiceProc.TimerPoll = (CTask::PFTimerPoll)&CLoginRecverTask::OnServiceTimer;
    AddRuleProc(Service, cServiceProc);

    NextState(WaitLogin);
}

u32 CLoginRecverTask::OnWaitLogin(CMessage *const pcMsg)
{
    u32 dwRet = PROCMSG_FAIL;
    switch(pcMsg->event)
    {
    case DEV_LOGIN_REQ:
        {
            //服务器收到登录请求
            u32 dwErrorCode = CMS_SUCCESS;
            COspSipMsg* pcOspSipMsg = (COspSipMsg*)pcMsg->content;
            //不再检查pcOspSipMsg是否为空，如果为空就直接崩溃吧

            CDevLoginReq cReq;
            pcOspSipMsg->GetMsgBody(cReq);
            SetDevUri(cReq.GetDevUri());
            SetDevType(cReq.GetDevType());
            SetDevAddrList(cReq.GetDevAddrList());

            SetServerUri(OspSipGetLocalURI().GetURIString());

            TASKLOG(EVENT_LEV, "设备[%s][%s]向服务器[%s]发起登录请求\n",
                GetDevType().c_str(), GetDevUri().c_str(), GetServerUri().c_str());

            //DevUri合法性检查
            do 
            {   
                if (!GetDevSipUri().IsValidURI())   //无效的SIP URI
                {
                    TASKLOG(ERROR_LEV, "无效dev uri[%s]登录服务器[%s]\n",
                        GetDevUri().c_str(), GetServerUri().c_str());
                    dwErrorCode = ERR_CMU_CMU_INVALID_CMU_URI;
                    break;
                }

                if (g_cGeneralLoginList.Exist(GetDevUri()))   //重复登录
                {
                    TASKLOG(ERROR_LEV, "已存在dev uri[%s]登录服务器[%s]\n",
                        GetDevUri().c_str(), GetServerUri().c_str());
                    dwErrorCode = ERR_CMU_CMU_ALREADY_CONN;
                    break;
                }
            } 
            while (0); 

            //生成登录session, 直接使用设备URI
            SetSession(GetDevUri());

            CDevLoginRsp cRsp;
            cRsp.SetHeadFromReq(cReq);
            cRsp.SetSession(GetSession());
            cRsp.SetErrorCode(dwErrorCode);
            PostMsgRsp(pcOspSipMsg->GetSipTransID(), cRsp);

            if (dwErrorCode == CMS_SUCCESS)
            {
                //登录成功
                TASKLOG(CRITICAL_LEV, "设备[%s][%s]登录成功!\n",
                    GetDevType().c_str(), GetDevUri().c_str());

                //添加到Dev列表中
                g_cGeneralLoginList.Insert(GetDevUri(), this);

                //开始心跳
                SetHBParam(GetDevSipUri());

                //进入服务态
                NextState(Service);

                dwRet = PROCMSG_OK;
            }
            else
            {
                //注册失败
                TASKLOG(CRITICAL_LEV, "设备[%s][%s]登录失败\n",
                    GetDevType().c_str(), GetDevUri().c_str());

                dwRet = PROCMSG_DEL;
            }
        }
        break;

    default:
        {
            TASKLOG(WARNING_LEV, "Receive unkown Msg[%s-%d]\n", OspExtEventDesc(pcMsg->event).c_str(), pcMsg->event);
        }
        break;
    }

    return dwRet;
}

u32 CLoginRecverTask::OnWaitLoginTimer()
{
    return TIMERPOLL_DONE;
}

u32 CLoginRecverTask::OnService(CMessage *const pcMsg)
{
    u32 dwRet = PROCMSG_FAIL;
    switch(pcMsg->event)
    {
    case DEV_LOGOUT_REQ:
        {
            //服务器收到登出请求
            COspSipMsg* pcOspSipMsg = (COspSipMsg*)pcMsg->content;
            //不再检查pcOspSipMsg是否为空，如果为空就直接崩溃吧

            CDevLogoutReq cReq;
            pcOspSipMsg->GetMsgBody(cReq);

            CDevLogoutRsp cRsp;
            cRsp.SetHeadFromReq(cReq);
            cRsp.SetErrorCode(CMS_SUCCESS);
            PostMsgRsp(pcOspSipMsg->GetSipTransID(), cRsp);

            //登出成功
            TASKLOG(CRITICAL_LEV, "设备[%s][%s]登出成功!\n",
                GetDevType().c_str(), GetDevUri().c_str());

            //停止心跳
            StopHB(GetDevSipUri());

            //删除登录会话
            dwRet = PROCMSG_DEL;
        }
        break;

    case OSP_SIP_DISCONNECT:
        {
            TASKLOG(CRITICAL_LEV, "dev[%s]和server[%s]断链\n", 
                GetDevUri().c_str(), GetServerUri().c_str());

            dwRet = PROCMSG_DEL;
        }
        break;

    default:
        {
            TASKLOG(WARNING_LEV, "Receive unkown Msg[%s-%d]\n", OspExtEventDesc(pcMsg->event).c_str(), pcMsg->event);
        }
        break;
    }

    return dwRet;
}

u32 CLoginRecverTask::OnServiceTimer()
{
    return TIMERPOLL_DONE;
}

//////////////////////////////////////////////////////////////////////////////////////////////

void CGeneralLoginList::PrintData()
{
    Iterator pPos;
    string strSession;
    CLoginTask* pTask = NULL;

    OspPrintf(TRUE, FALSE, "当前在线的dev：[%u]个\n", GetSize());    
    u32 i = 0;

    while (!pPos.End())
    {
        if (this->Iterate(pPos, strSession, pTask) && pTask != NULL)
        {
            i++;
            OspPrintf(TRUE, FALSE, "  %4u. dev[%s-%s]-session[%s]\n", i, 
                pTask->GetDevType().c_str(),
                pTask->GetDevUri().c_str(),
                pTask->GetSession().c_str());
        }

        pTask = NULL;
    }

    OspPrintf(TRUE, FALSE, "当前在线的cu：[%u]个\n", GetSize());
}
