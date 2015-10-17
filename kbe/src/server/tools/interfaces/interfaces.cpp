/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2016 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "orders.h"
#include "interfaces.h"
#include "interfaces_tasks.h"
#include "anonymous_channel.h"
#include "interfaces_interface.h"
#include "network/common.h"
#include "network/tcp_packet.h"
#include "network/udp_packet.h"
#include "network/message_handler.h"
#include "thread/threadpool.h"
#include "server/components.h"

#include "baseapp/baseapp_interface.h"
#include "cellapp/cellapp_interface.h"
#include "baseappmgr/baseappmgr_interface.h"
#include "cellappmgr/cellappmgr_interface.h"
#include "loginapp/loginapp_interface.h"
#include "dbmgr/dbmgr_interface.h"
#include "profile.h"	

namespace KBEngine{
	
ServerConfig g_serverConfig;
KBE_SINGLETON_INIT(Interfaces);

class ScriptTimerHandler : public TimerHandler
{
public:
	ScriptTimerHandler(ScriptTimers* scriptTimers, PyObject * callback) :
		pyCallback_(callback),
		scriptTimers_(scriptTimers)
	{
	}

	~ScriptTimerHandler()
	{
		Py_DECREF(pyCallback_);
	}

private:
	virtual void handleTimeout(TimerHandle handle, void * pUser)
	{
		int id = ScriptTimersUtil::getIDForHandle(scriptTimers_, handle);

		PyObject *pyRet = PyObject_CallFunction(pyCallback_, "i", id);
		if (pyRet == NULL)
		{
			SCRIPT_ERROR_CHECK();
			return;
		}
		return;
	}

	virtual void onRelease(TimerHandle handle, void * /*pUser*/)
	{
		scriptTimers_->releaseTimer(handle);
		delete this;
	}

	PyObject* pyCallback_;
	ScriptTimers* scriptTimers_;
};



//-------------------------------------------------------------------------------------
Interfaces::Interfaces(Network::EventDispatcher& dispatcher, 
			 Network::NetworkInterface& ninterface, 
			 COMPONENT_TYPE componentType,
			 COMPONENT_ID componentID):
	PythonApp(dispatcher, ninterface, componentType, componentID),
	mainProcessTimer_(),
	reqCreateAccount_requests_(),
	reqAccountLogin_requests_(),
	mutex_(),
	scriptTimers_()
{
	ScriptTimers::initialize(*this);
}

//-------------------------------------------------------------------------------------
Interfaces::~Interfaces()
{
	mainProcessTimer_.cancel();
	lockthread();

	if(reqCreateAccount_requests_.size() > 0)
	{	
		int i = 0;
		REQCREATE_MAP::iterator iter = reqCreateAccount_requests_.begin();
		for(; iter != reqCreateAccount_requests_.end(); ++iter)
		{
			WARNING_MSG(fmt::format("Interfaces::~Interfaces(): Discarding {0}/{1} reqCreateAccount[{2:p}] tasks.\n", 
				++i, reqCreateAccount_requests_.size(), (void*)iter->second));
		}
	}

	if(reqAccountLogin_requests_.size() > 0)
	{
		int i = 0;
		REQLOGIN_MAP::iterator iter = reqAccountLogin_requests_.begin();
		for(; iter != reqAccountLogin_requests_.end(); ++iter)
		{
			WARNING_MSG(fmt::format("Interfaces::~Interfaces(): Discarding {0}/{1} reqAccountLogin[{2:p}] tasks.\n", 
				++i, reqAccountLogin_requests_.size(), (void*)iter->second));
		}
	}

	unlockthread();
}

//-------------------------------------------------------------------------------------	
void Interfaces::onShutdownBegin()
{
	// ֪ͨ�ű�
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	SCRIPT_OBJECT_CALL_ARGS0(getEntryScript().get(), const_cast<char*>("onInterfaceAppShutDown"));

	scriptTimers_.cancelAll();
}

//-------------------------------------------------------------------------------------	
void Interfaces::onShutdownEnd()
{
	PythonApp::onShutdownEnd();
}

//-------------------------------------------------------------------------------------
void Interfaces::lockthread()
{
	mutex_.lockMutex();
}

//-------------------------------------------------------------------------------------
void Interfaces::unlockthread()
{
	mutex_.unlockMutex();
}

//-------------------------------------------------------------------------------------
void Interfaces::eraseOrders_s(std::string ordersid)
{
	lockthread();

	ORDERS::iterator iter = orders_.find(ordersid);
	if(iter != orders_.end())
	{
		ERROR_MSG(fmt::format("Interfaces::eraseOrders_s: chargeID={} not found!\n", ordersid));
	}

	orders_.erase(iter);
	unlockthread();
}

//-------------------------------------------------------------------------------------
bool Interfaces::hasOrders(std::string ordersid)
{
	bool ret = false;
	
	lockthread();
	ORDERS::iterator iter = orders_.find(ordersid);
	ret = (iter != orders_.end());
	unlockthread();
	
	return ret;
}

//-------------------------------------------------------------------------------------
bool Interfaces::run()
{
	return PythonApp::run();
}

//-------------------------------------------------------------------------------------
void Interfaces::handleTimeout(TimerHandle handle, void * arg)
{
	switch (reinterpret_cast<uintptr>(arg))
	{
		case TIMEOUT_TICK:
			this->handleMainTick();
			break;
		default:
			break;
	}

	PythonApp::handleTimeout(handle, arg);
}

//-------------------------------------------------------------------------------------
void Interfaces::handleMainTick()
{
	 //time_t t = ::time(NULL);
	 //DEBUG_MSG("Interfaces::handleGameTick[%"PRTime"]:%u\n", t, time_);
	
	g_kbetime++;
	threadPool_.onMainThreadTick();
	handleTimers();
	networkInterface().processChannels(&InterfacesInterface::messageHandlers);
}

//-------------------------------------------------------------------------------------
bool Interfaces::initializeBegin()
{
	return true;
}

//-------------------------------------------------------------------------------------
bool Interfaces::inInitialize()
{
	PythonApp::inInitialize();
	// �㲥�Լ��ĵ�ַ�������ϵ�����kbemachine
	Components::getSingleton().pHandler(this);
	return true;
}

//-------------------------------------------------------------------------------------
bool Interfaces::initializeEnd()
{
	mainProcessTimer_ = this->dispatcher().addTimer(1000000 / g_kbeSrvConfig.gameUpdateHertz(), this,
							reinterpret_cast<void *>(TIMEOUT_TICK));

	// ����Ƶ����ʱ���
	CLOSE_CHANNEL_INACTIVITIY_DETECTION();

	this->threadPool().addTask(new AnonymousChannel());

	if (!initDB())
		return false;

	SCOPED_PROFILE(SCRIPTCALL_PROFILE);

	// ���нű����������
	PyObject* pyResult = PyObject_CallMethod(getEntryScript().get(), 
										const_cast<char*>("onInterfaceAppReady"), 
										const_cast<char*>(""));

	if(pyResult != NULL)
		Py_DECREF(pyResult);
	else
		SCRIPT_ERROR_CHECK();

	return true;
}

//-------------------------------------------------------------------------------------		
void Interfaces::onInstallPyModules()
{
	PyObject * module = getScript().getModule();

	for (int i = 0; i < SERVER_ERR_MAX; i++)
	{
		if(PyModule_AddIntConstant(module, SERVER_ERR_STR[i], i))
		{
			ERROR_MSG( fmt::format("Interfaces::onInstallPyModules: Unable to set KBEngine.{}.\n", SERVER_ERR_STR[i]));
		}
	}

	APPEND_SCRIPT_MODULE_METHOD(module,		chargeResponse,					__py_chargeResponse,					METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(module,		accountLoginResponse,			__py_accountLoginResponse,				METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(module,		createAccountResponse,			__py_createAccountResponse,				METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(module,		addTimer,						__py_addTimer,							METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(module,		delTimer,						__py_delTimer,							METH_VARARGS,			0);

}

//-------------------------------------------------------------------------------------		
bool Interfaces::initDB()
{
	return true;
}

//-------------------------------------------------------------------------------------
void Interfaces::finalise()
{
	ScriptTimers::finalise(*this);
	PythonApp::finalise();
}

//-------------------------------------------------------------------------------------
void Interfaces::reqCreateAccount(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	std::string registerName, accountName, password, datas;
	COMPONENT_ID cid;
	uint8 accountType = 0;

	s >> cid >> registerName >> password >> accountType;
	s.readBlob(datas);
	
	if(accountType == (uint8)ACCOUNT_TYPE_MAIL)
	{
	}

	lockthread();

	REQCREATE_MAP::iterator iter = reqCreateAccount_requests_.find(registerName);
	if(iter != reqCreateAccount_requests_.end())
	{
		unlockthread();
		return;
	}

	CreateAccountTask* pinfo = new CreateAccountTask();
	pinfo->commitName = registerName;
	pinfo->accountName = registerName;
	pinfo->getDatas = "";
	pinfo->password = password;
	pinfo->postDatas = datas;
	pinfo->retcode = SERVER_ERR_OP_FAILED;
	pinfo->baseappID = cid;
	pinfo->dbmgrID = pChannel->componentID();
	pinfo->address = pChannel->addr();
	pinfo->enable = true;

	reqCreateAccount_requests_[pinfo->commitName] = pinfo;
	unlockthread();

	// phw: ����ʹ��������Ľű��ص�����˲��ٰѴ����ݷŵ��߳���������д���
	//this->threadPool().addTask(pinfo);

	// �������ɽű�����
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	PyObject* pyResult = PyObject_CallMethod(getEntryScript().get(), 
										const_cast<char*>("requestCreateAccount"), 
										const_cast<char*>("ssy#"), 
										registerName.c_str(), 
										password.c_str(),
										datas.c_str(), datas.length());

	if(pyResult != NULL)
		Py_DECREF(pyResult);
	else
		SCRIPT_ERROR_CHECK();
}

//-------------------------------------------------------------------------------------
void Interfaces::createAccountResponse(std::string commitName, std::string realAccountName, std::string extraDatas, KBEngine::SERVER_ERROR_CODE errorCode)
{
	REQCREATE_MAP::iterator iter = reqCreateAccount_requests_.find(commitName);
	if (iter == reqCreateAccount_requests_.end())
	{
		// �����ϲ������Ҳ�������������Ҳ��������Ǹ��ֲܿ������飬����д��־��¼����
		ERROR_MSG(fmt::format("Interfaces::createAccountResponse: accountName '{}' not found!" \
			"realAccountName = '{}', extra datas = '{}', error code = '{}'", 
			commitName, 
			realAccountName, 
			extraDatas, 
			errorCode));
		return;
	}

	CreateAccountTask *task = iter->second;

	Network::Bundle* pBundle = Network::Bundle::ObjPool().createObject();

	(*pBundle).newMessage(DbmgrInterface::onCreateAccountCBFromInterfaces);
	(*pBundle) << task->baseappID << commitName << realAccountName << task->password << errorCode;

	(*pBundle).appendBlob(task->postDatas);
	(*pBundle).appendBlob(extraDatas);

	Network::Channel* pChannel = Interfaces::getSingleton().networkInterface().findChannel(task->address);

	if(pChannel)
	{
		pChannel->send(pBundle);
	}
	else
	{
		ERROR_MSG(fmt::format("Interfaces::createAccountResponse: not found channel. commitName={}\n", commitName));
		Network::Bundle::ObjPool().reclaimObject(pBundle);
	}

	// ����
	reqCreateAccount_requests_.erase(iter);
	delete task;
}

//-------------------------------------------------------------------------------------
PyObject* Interfaces::__py_createAccountResponse(PyObject* self, PyObject* args)
{
	int argCount = PyTuple_Size(args);

	const char *commitName;
	const char *realAccountName;
	Py_buffer extraDatas;
	KBEngine::SERVER_ERROR_CODE errCode;

	if (!PyArg_ParseTuple(args, "ssy*H", &commitName, &realAccountName, &extraDatas, &errCode))
		return NULL;

	Interfaces::getSingleton().createAccountResponse(std::string(commitName),
		std::string(realAccountName),
		std::string((const char *)extraDatas.buf, extraDatas.len),
		errCode);

	PyBuffer_Release(&extraDatas);
	S_Return;
}

//-------------------------------------------------------------------------------------
void Interfaces::onAccountLogin(Network::Channel* pChannel, KBEngine::MemoryStream& s) 
{
	std::string loginName, accountName, password, datas;
	COMPONENT_ID cid;

	s >> cid >> loginName >> password;
	s.readBlob(datas);

	lockthread();

	REQLOGIN_MAP::iterator iter = reqAccountLogin_requests_.find(loginName);
	if(iter != reqAccountLogin_requests_.end())
	{
		unlockthread();
		return;
	}

	LoginAccountTask* pinfo = new LoginAccountTask();
	pinfo->commitName = loginName;
	pinfo->accountName = loginName;
	pinfo->getDatas = "";
	pinfo->password = password;
	pinfo->postDatas = datas;
	pinfo->retcode = SERVER_ERR_OP_FAILED;
	pinfo->baseappID = cid;
	pinfo->dbmgrID = pChannel->componentID();
	pinfo->address = pChannel->addr();
	pinfo->enable = true;

	reqAccountLogin_requests_[pinfo->commitName] = pinfo;
	unlockthread();

	// phw: ����ʹ��������Ľű��ص�����˲��ٰѴ����ݷŵ��߳���������д���
	//this->threadPool().addTask(pinfo);

	// �������ɽű�����
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	PyObject* pyResult = PyObject_CallMethod(getEntryScript().get(), 
										const_cast<char*>("requestAccountLogin"), 
										const_cast<char*>("ssy#"), 
										loginName.c_str(), 
										password.c_str(), 
										datas.c_str(), datas.length());

	if(pyResult != NULL)
		Py_DECREF(pyResult);
	else
		SCRIPT_ERROR_CHECK();
}

//-------------------------------------------------------------------------------------
void Interfaces::accountLoginResponse(std::string commitName, std::string realAccountName, std::string extraDatas, KBEngine::SERVER_ERROR_CODE errorCode)
{
	REQLOGIN_MAP::iterator iter = reqAccountLogin_requests_.find(commitName);
	if (iter == reqAccountLogin_requests_.end())
	{
		// �����ϲ������Ҳ�������������Ҳ��������Ǹ��ֲܿ������飬����д��־��¼����
		ERROR_MSG(fmt::format("Interfaces::accountLoginResponse: commitName '{}' not found!" \
			"realAccountName = '{}', extra datas = '{}', error code = '{}'", 
			commitName, 
			realAccountName, 
			extraDatas, 
			errorCode));
		return;
	}

	LoginAccountTask *task = iter->second;

	Network::Bundle* pBundle = Network::Bundle::ObjPool().createObject();
	
	(*pBundle).newMessage(DbmgrInterface::onLoginAccountCBBFromInterfaces);
	(*pBundle) << task->baseappID << commitName << realAccountName << task->password << errorCode;

	(*pBundle).appendBlob(task->postDatas);
	(*pBundle).appendBlob(extraDatas);

	Network::Channel* pChannel = Interfaces::getSingleton().networkInterface().findChannel(task->address);

	if(pChannel)
	{
		pChannel->send(pBundle);
	}
	else
	{
		ERROR_MSG(fmt::format("Interfaces::accountLoginResponse: not found channel. commitName={}\n", commitName));
		Network::Bundle::ObjPool().reclaimObject(pBundle);
	}

	// ����
	reqAccountLogin_requests_.erase(iter);
	delete task;
}

//-------------------------------------------------------------------------------------
PyObject* Interfaces::__py_accountLoginResponse(PyObject* self, PyObject* args)
{
	int argCount = PyTuple_Size(args);

	const char *commitName;
	const char *realAccountName;
	Py_buffer extraDatas;
	KBEngine::SERVER_ERROR_CODE errCode;

	if (!PyArg_ParseTuple(args, "ssy*H", &commitName, &realAccountName, &extraDatas, &errCode))
		return NULL;

	Interfaces::getSingleton().accountLoginResponse(std::string(commitName),
		std::string(realAccountName),
		std::string((const char *)extraDatas.buf, extraDatas.len),
		errCode);

	PyBuffer_Release(&extraDatas);
	S_Return;
}

//-------------------------------------------------------------------------------------
void Interfaces::charge(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	OrdersCharge* pOrdersCharge = new OrdersCharge();

	pOrdersCharge->timeout = timestamp()  + uint64(g_kbeSrvConfig.interfaces_orders_timeout_ * stampsPerSecond());

	pOrdersCharge->dbmgrID = pChannel->componentID();
	pOrdersCharge->address = pChannel->addr();

	s >> pOrdersCharge->baseappID;
	s >> pOrdersCharge->ordersID;
	s >> pOrdersCharge->dbid;
	s.readBlob(pOrdersCharge->postDatas);
	s >> pOrdersCharge->cbid;

	INFO_MSG(fmt::format("Interfaces::charge: componentID={4}, chargeID={0}, dbid={1}, cbid={2}, datas={3}!\n",
		pOrdersCharge->ordersID, pOrdersCharge->dbid, pOrdersCharge->cbid, pOrdersCharge->postDatas, pOrdersCharge->baseappID));

	lockthread();

	ORDERS::iterator iter = orders_.find(pOrdersCharge->ordersID);
	if(iter != orders_.end())
	{
		ERROR_MSG(fmt::format("Interfaces::charge: chargeID={} is exist!\n", pOrdersCharge->ordersID));
		delete pOrdersCharge;
		unlockthread();
		return;
	}

	ChargeTask* pinfo = new ChargeTask();
	pinfo->orders = *pOrdersCharge;
	pinfo->pOrders = pOrdersCharge;
	orders_[pOrdersCharge->ordersID].reset(pOrdersCharge);
	unlockthread();
	
	// phw: ����ʹ��������Ľű��ص�����˲��ٰѴ����ݷŵ��߳���������д���
	//this->threadPool().addTask(pinfo);

	// �������ɽű�����
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	PyObject* pyResult = PyObject_CallMethod(getEntryScript().get(), 
										const_cast<char*>("requestCharge"), 
										const_cast<char*>("sKy#"), 
										pOrdersCharge->ordersID.c_str(),
										pOrdersCharge->dbid, 
										pOrdersCharge->postDatas.c_str(), pOrdersCharge->postDatas.length());

	if(pyResult != NULL)
		Py_DECREF(pyResult);
	else
		SCRIPT_ERROR_CHECK();
}

//-------------------------------------------------------------------------------------
void Interfaces::chargeResponse(std::string orderID, std::string extraDatas, KBEngine::SERVER_ERROR_CODE errorCode)
{
	ORDERS::iterator iter = orders_.find(orderID);
	if (iter == orders_.end())
	{
		// �����ϲ������Ҳ�������������Ҳ��������Ǹ��ֲܿ������飬����д��־��¼����
		ERROR_MSG(fmt::format("Interfaces::chargeResponse: order id '{}' not found! extra datas = '{}', error code = '{}'", 
			orderID, 
			extraDatas, 
			errorCode));
		return;
	}

	std::shared_ptr<Orders> orders = iter->second;
	Network::Bundle* pBundle = Network::Bundle::ObjPool().createObject();

	(*pBundle).newMessage(DbmgrInterface::onChargeCB);
	(*pBundle) << orders->baseappID << orders->ordersID << orders->dbid;
	(*pBundle).appendBlob(orders->getDatas);
	(*pBundle) << orders->cbid;
	(*pBundle) << errorCode;

	Network::Channel* pChannel = networkInterface().findChannel(orders->address);

	if(pChannel)
	{
		WARNING_MSG(fmt::format("Interfaces::chargeResponse: orders={} commit is failed!\n", 
			orders->ordersID));

		pChannel->send(pBundle);
	}
	else
	{
		ERROR_MSG(fmt::format("Interfaces::chargeResponse: not found channel. orders={}\n", 
			orders->ordersID));

		Network::Bundle::ObjPool().reclaimObject(pBundle);
	}

	orders_.erase(iter);
}

//-------------------------------------------------------------------------------------
PyObject* Interfaces::__py_chargeResponse(PyObject* self, PyObject* args)
{
	int argCount = PyTuple_Size(args);

	const char *orderID;
	Py_buffer extraDatas;
	KBEngine::SERVER_ERROR_CODE errCode;

	if (!PyArg_ParseTuple(args, "sy*H", &orderID, &extraDatas, &errCode))
		return NULL;

	if (errCode < SERVER_ERR_MAX)
	{
		Interfaces::getSingleton().chargeResponse(std::string(orderID),
			std::string((const char *)extraDatas.buf, extraDatas.len),
			errCode);
	}
	else
	{
		//ERROR_MSG(fmt::format());
	}

	PyBuffer_Release(&extraDatas);
	S_Return;
}

//-------------------------------------------------------------------------------------
void Interfaces::eraseClientReq(Network::Channel* pChannel, std::string& logkey)
{
	lockthread();

	REQCREATE_MAP::iterator citer = reqCreateAccount_requests_.find(logkey);
	if(citer != reqCreateAccount_requests_.end())
	{
		citer->second->enable = false;
		DEBUG_MSG(fmt::format("Interfaces::eraseClientReq: reqCreateAccount_logkey={} set disabled!\n", logkey));
	}

	REQLOGIN_MAP::iterator liter = reqAccountLogin_requests_.find(logkey);
	if(liter != reqAccountLogin_requests_.end())
	{
		liter->second->enable = false;
		DEBUG_MSG(fmt::format("Interfaces::eraseClientReq: reqAccountLogin_logkey={} set disabled!\n", logkey));
	}

	unlockthread();
}

//-------------------------------------------------------------------------------------
PyObject* Interfaces::__py_addTimer(float interval, float repeat, PyObject *callback)
{
	if (!PyCallable_Check(callback))
	{
		PyErr_Format(PyExc_TypeError, "Interfaces::addTimer: '%.200s' object is not callable", callback->ob_type->tp_name);
		PyErr_PrintEx(0);
		return NULL;
	}

	ScriptTimers * pTimers = &Interfaces::getSingleton().scriptTimers();
	ScriptTimerHandler *handler = new ScriptTimerHandler(pTimers, callback);

	int id = ScriptTimersUtil::addTimer(&pTimers, interval, repeat, (int)&callback, handler);

	if (id == 0)
	{
		delete handler;
		PyErr_SetString(PyExc_ValueError, "Unable to add timer");
		PyErr_PrintEx(0);
		return NULL;
	}

	Py_INCREF(callback);
	return PyLong_FromLong(id);
}

PyObject* Interfaces::__py_delTimer(ScriptID timerID)
{
	if (!ScriptTimersUtil::delTimer(&Interfaces::getSingleton().scriptTimers(), timerID))
	{
		return PyLong_FromLong(-1);
	}

	return PyLong_FromLong(timerID);
}

}
