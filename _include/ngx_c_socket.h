﻿
#ifndef __NGX_SOCKET_H__
#define __NGX_SOCKET_H__

#include <vector>       //vector
#include <list>         //list
#include <sys/epoll.h>  //epoll
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <pthread.h>    //多线程
#include <semaphore.h>  //信号量 
#include <atomic>       //c++11里的原子操作
#include <map>          //multimap

#include "ngx_comm.h"
#include "TimeWheel.h"

//一些宏定义放在这里-----------------------------------------------------------------------
#define NGX_LISTEN_BACKLOG  4096	   //已完成连接队列，限制为511，
#define NGX_MAX_EVENTS      1024  //epoll_wait一次最多接收这么多个事件，
							      //nginx中缺省是512

typedef struct ngx_listening_s   ngx_listening_t, *lpngx_listening_t;
typedef struct ngx_connection_s  ngx_connection_t, *lpngx_connection_t;
typedef class  CSocket           CSocket;

//定义成员函数指针
typedef void (CSocket::*ngx_event_handler_pt)(lpngx_connection_t pConn);

//一些专用结构定义放在这里，暂时不考虑放ngx_global.h里了
struct ngx_listening_s  //和监听端口有关的结构
{
	int		           port;         //监听的端口号
	int		           fd;           //套接字句柄socket
	lpngx_connection_t p_connection; //连接池中的一个连接，注意这是个指针
};



struct MESSAGE_QUEUE2 //for handing connections
{
	lpngx_connection_t head2;
	lpngx_connection_t tail2;
	int size2;
};

struct CONN_EVENT //used for handling connection events, needing to be 
				  //dynamically allocated 
{
	CONN_EVENT* prev;
	CONN_EVENT* next;
	eventConnType eType;
	lpngx_connection_t pConn;
};

struct CONN_EVENT_QUEUE //connection event queues
{
	CONN_EVENT* phead;
	CONN_EVENT* ptail;
	int size;
};

struct IOthreadCache
{
	int epollHandle; //该线程的epoll FD
	int connCount; //该线程epoll所用的客户端节点数量
	int evtfd; //创建eventfd文件描述，用于其他线程对该线程epollHandle下连接的事件通知
	lpngx_connection_t pConnEFD; //指向eFD所绑定的连接
	CONN_EVENT_QUEUE connEventQueue; //链接该IO线程的所有连接事件
	int atomicLock;	//保证connEventQueue的线程安全
};

//消息头，引入的目的是当收到数据包时，额外记录一些内容，以及消息队列使用
typedef struct _STRUC_MSG_HEADER
{
	_STRUC_MSG_HEADER* preMsg;       //消息队列中用于连接各条消息
	_STRUC_MSG_HEADER* nextMsg;       //消息队列中用于连接各条消息
	lpngx_connection_t pConn;         //记录对应的连接，注意这是个指针
	unsigned long      iCurrsequence; //收到数据包时记录对应连接的序号，
									  //将来用于比较是否连接已经作废
}STRUC_MSG_HEADER, * LPSTRUC_MSG_HEADER;

struct MESSAGE_QUEUE //for handling messages
{
	LPSTRUC_MSG_HEADER head;
	LPSTRUC_MSG_HEADER tail;
	int size;
	//int atomicLock;
};

//struct MESSAGE_QUEUE;
//以下三个结构是非常重要的三个结构，我们遵从官方nginx的写法；
//该结构表示一个TCP连接(客户端主动发起的、Nginx服务器被动接受的TCP连接)
struct ngx_connection_s
{
	ngx_connection_s();                  //构造函数
	virtual ~ngx_connection_s();         //析构函数
	void GetOneToUse();                  //分配出去的时候初始化一些内容
	void PutOneToFree();                 //回收回来的时候做一些事情

	IOthreadCache*       pIOthread;      //该连接所关联的IOthreadCache
	int                  fd;             //套接字句柄socket
	lpngx_listening_t    listening;      //如果这个连接被分配了一个监听套接字，
									     //listening就指向监听套接字对应的那个
									     //lpngx_listening_t的内存首地址		

	//-----------------------------------------------------------------------------------	
	unsigned             instance : 1;   //(位域)失效标志位：0有效，1失效，
									     //在EpollProcessIO()中有用 
	std::atomic<unsigned long> iCurrsequence; //该变量用于检测错包废包
	struct sockaddr      s_sockaddr;     //保存对方地址信息用的
	char                 addr_text[100]; //地址的文本信息，100足够，如果是ipv4地址，
										 //192.168.0.119:65535，其实只需30字节就够

	//和读有关的标志----------------------------------------------------------------------
	ngx_event_handler_pt rhandler;       //读事件的相关处理方法
	ngx_event_handler_pt whandler;       //写事件的相关处理方法

	//和epoll事件有关---------------------------------------------------------------------
	uint32_t             events;         //和epoll事件有关

	//和收包有关--------------------------------------------------------------------------
	unsigned char    curStat;                           //当前收包的状态
	char             dataHeadInfo[_PKG_HEADER_BUFSIZE]; //包头存储缓存
	char*			 precvbuf_hstar;					//包头在包头存储缓存中的
														//起始地址，读包头时使用
	char*            precvbuf;                          //接收数据的缓冲区的头指针，
												        //对收到不全的包非常有用
	unsigned int     irecvlen;                          //指定要收到多少数据，和
														//precvbuf配套使用
	char*            precvMemPointer;                   //new出来的用于存放收包的
														//内存首地址
	int				 wrongPKGAdmit;				        //允许校验错误包次数

	char*            pRecvBuff;                         //指向该连接的数据接收缓冲区
	//int              buffLength;					    //接收缓冲区剩余数据长度
	char*			 recvMsgHead;			            //数据接收缓冲区一条消息的头
	char*            recvMsgRead;                       //读取数据接收缓冲区一条消息 
	MESSAGE_QUEUE     unpackMsgQue;						//解包消息队列，争对每次收数据包

	pthread_mutex_t  logicPorcMutex;                    //逻辑处理相关的互斥量

	//和发包有关--------------------------------------------------------------------------
	int	  sendBufFull;        //发送消息，如果发送缓冲区满了，则需要通过epoll事件来驱
							 //动消息的继续发送，所以如果发送缓冲区满，则用这个变量标记
	char* psendMemPointer;    //发送完成后释放用的，整个数据的头指针，
	                          //其实是 消息头 + 包头 + 包体
	char* psendbuf;           //发送数据的缓冲区的头指针，其实是包头+包体
	unsigned int  isendlen;   //要发送多少数据

	//和收发包顺序有关--------------------------------------------------------------------
	//=============================================================================================
	/*std::atomic<unsigned int> recvdMsgCount;*/ //当一个连接建立后，服务器从客户端一共
											 //收到的消息数量(已放入消息队列待处理)
	/*std::atomic<unsigned int> sentMsgCount;*/  //当一个连接建立后，服务器返回给客户端
											 //的总消息数量(成功发送到send buffer)
	//=============================================================================================

	//和网络安全有关----------------------------------------------------------------------
	/*uint64_t         FloodkickLastTime;*/  //Flood攻击上次收到包的时间
	/*int              FloodAttackCount;*/   //Flood攻击在该时间内收到包的次数统计
	int              sendCount;          //该连接在发送队列中有的数据条目数，若client只发不收，
	                                     //则可能造成此数过大，依据此数做出踢出处理
	//和定时器有关------------------------------------------------------------------------
	//1 连接回收 2 心跳包检查
	TIMER_NODE*      timerEntryRecy;
	TIMER_NODE*      timerEntryPing;
	int              timerStatus; // 0进入idle状态；1进入回收状态，直接回收到自由链表
	int				 eventFlags[E_MAX_TYPE]; //表示连接的各个事件是否进入待定，
													//即进入连接队列中
	//pthread_mutex_t  recyConnMutex;
	//----------------------------------------------------------------------------
	lpngx_connection_t   nextConn;       //这是个指针，等价于传统链表里的next成员：
										 //后继指针，指向下一个本类型对象，用于把空
										 //闲的连接池对象串联起来构成一个单向链表，
										 //方便取用
};

//socket类
class CSocket
{
	//声明 struct ngx_connection_s 是 class CSocket 的友元类
	friend struct ngx_connection_s;

public:
	CSocket();                              //构造函数
	virtual ~CSocket();                     //释放函数 
	virtual bool Initialize();              //初始化函数[父进程中执行]
	virtual bool Initialize_subproc();      //初始化函数[子进程中执行]
	virtual void Shutdown_subproc();        //关闭退出函数[子进程中执行]

public:
	//=========================================================================
	char* outMsgRecvQueue();                        //将一个消息取出消息队列	
	//=========================================================================
	virtual void threadRecvProcFunc(char* pMsgBuf); //处理客户端请求，虚函数，因为
													//将来可能写子类继承本类
	
	//virtual void procPingTimeOutChecking(
	//	LPSTRUC_MSG_HEADER tmpmsg, 
	//	time_t cur_time);                   //检测心跳包是否超时的事宜，父类本函数
	//                                        //只是把内存释放，子类应重新实现该函数

public:
	int ngx_epoll_init(); //epoll功能初始化
	int ngx_epoll_oper_event(int epollHandle, int fd, uint32_t eventtype, uint32_t flag,
		int bcaction, lpngx_connection_t pConn); //epoll操作事件
	int EpollProcessIO(int epollHandle, //epoll等待接收和处理IO
		struct epoll_event myEvents[], int maxEvents, int timer);
	void EpollProcessEvent(IOthreadCache* pIOThread); //处理epoll事件

	bool SetandStartPrint(void); //设置屏幕打印消息

protected:
	//=========================================================================
	//把数据扔到待发送对列中 
	void msgSend(char* psendbuf); 
	//=========================================================================
	//从sendingQueue消息队列中弹出指定消息
	void popSendingQueueMsg(LPSTRUC_MSG_HEADER msg);
	//=========================================================================
	//关闭一个已经成功建立的连接
	//void ngx_close_running_connection(lpngx_connection_t p_Conn); 

//private:
	void ReadConf();                          //专门用于读各种配置项
	bool ngx_open_listening_sockets();        //监听必须的端口，支持多个端口
	void ngx_close_listening_sockets();       //关闭监听套接字
	bool setnonblocking(int sockfd);          //设置非阻塞套接字
	//通知并唤醒该连接所在的epoll，该连接有事件需要处理
	void ThreadEventNotify(lpngx_connection_t pConn); 
	//epoll被唤醒后，调用该连接的, EventRespond()读取收到的通知											
	void ThreadEventRespond(lpngx_connection_t pConn);
												
	//一些业务处理函数handler
	//建立新连接
	void ngx_event_accept(lpngx_connection_t pConnL);
	//设置数据来时的读处理函数
	void ngx_read_request_handler(lpngx_connection_t pConn);
	//设置数据发送时的写处理函数
	void ngx_write_request_handler(lpngx_connection_t pConn);
	//对于epoll事件的空处理函数q
	void ngx_null_request_handler(lpngx_connection_t pConn) {}

	//通用连接关闭函数，资源用这个函数释放(因为涉及多个要释放的资源，所以写成函数)
	void ngx_close_connection(lpngx_connection_t c);

	//used to release message queues of MESSAGE_QUEUE type;
	//in some scenarios may need critical lock applied by the caller
	void MsgQueueRelease(MESSAGE_QUEUE* pMsgQueue);
	//接收从客户端来的数据专用函数
	ssize_t recvproc(lpngx_connection_t pConn, char* buff, ssize_t buflen);
	//包头收完整后的处理，先进行数据包校验，筛查畸形包，恶意包或错误包
	void ngx_read_request_handler_PKGcheck(lpngx_connection_t pConn);
	//数据包校验和检查通过的包，进行包处理阶段1：写成函数，方便复用
	void ngx_read_request_handler_proc_pfirst(lpngx_connection_t pConn);
	//收到一个完整包后的处理，放到一个函数中，方便调用
	void ngx_read_request_handler_proc_plast(lpngx_connection_t pConn);
	//收到一个完整消息后，入消息队列
	//=========================================================================
	void inMsgRecvQueue(/*char* buf*/MESSAGE_QUEUE* pMsgQueue);
	//=========================================================================
	//清理接收消息队列
	void clearMsgRecvQueue();
	//清理发送消息队列
	void clearMsgSendQueue();

	//将数据发送到客户端
	ssize_t sendproc(lpngx_connection_t pConn, char* buff, ssize_t size);

	//获取对端信息相关，根据参数1给定的信息，获取地址端口字符串，返回这个字符串的长度
	size_t ngx_sock_ntop(struct sockaddr* sa, int port, u_char* text, size_t len);

	//连接池或连接相关
	//从连接池中获取一个空闲连接
	lpngx_connection_t ngx_get_connection(int isock);
	//归还参数pConn所指连接到连接池中
	void ngx_free_connection(lpngx_connection_t pConn);
	//将要回收的连接放到一个队列中来
	void ngx_recycle_connection(lpngx_connection_t pConn);
	void initconnection(); //初始化连接池
	void clearconnection(); //回收连接池

	bool ConnListProtection(); //连接对象池和客户连入控制

	////和时间相关的函数
	////设置踢出时钟(向map表中增加内容)
	//void AddToTimerQueue(lpngx_connection_t pConn);                    
	////从multimap中取得最早的时间返回去
	//time_t  GetEarliestTime(); 
	////从m_timeQueuemap移除最早的时间，并把最早这个时间所在的项的值所对应的指针返回，
	////调用者负责互斥，所以本函数不用互斥
	//LPSTRUC_MSG_HEADER RemoveFirstTimer();    
	////根据给的当前时间，从m_timeQueuemap找到比这个时间更早的1个节点返回去，
	////这些节点都是时间超过要处理的节点
	//LPSTRUC_MSG_HEADER GetOverTimeTimer(time_t cur_time);                 
	////把指定用户tcp连接从timer表中抠出去
	//void DeleteFromTimerQueue(lpngx_connection_t pConn);                  
	////清理时间队列中所有内容
	//void clearAllFromTimerQueue();                                        

	//线程相关函数(静态函数)
	//专门用来发送数据的线程
	static void* ServerSendQueueThread(void* threadData);
	//专门用来回收连接的线程
	//static void* ServerRecyConnThread(void* threadData); 
	//专门用来处理IO(收数据，发数据)的线程
	static void* ServerIOThread(void* threadData);

protected:
	//一些和网络通讯有关的成员变量
	size_t		lenPkgHeader;	//sizeof(COMM_PKG_HEADER);		
	size_t      lenMsgHeader;    //sizeof(STRUC_MSG_HEADER);

	//定时器专用函数
	static void PrintInfo(void* pConnVoid);
	static void SetConnToIdle(void* pConnVoid);
	static void PingTimeout(void* pConnVoid);

private:
	struct ThreadItem
	{
		pthread_t   _Handle;	  //线程句柄
		CSocket*    _pThis;       //记录线程池的指针	
		//int         _epollHandle;
		IOthreadCache* _pIOThread;
		bool        _ifrunning;    //标记是否正式启动起来，启动起来后，
								  //才允许调用StopAll()来释放
		//构造函数
		ThreadItem(CSocket* pthis) :
			_pThis(pthis), _pIOThread(NULL), _ifrunning(false){}
		//析构函数
		~ThreadItem() {}
	};

	static IOthreadCache* IOThreads;

	TIMER_NODE* pPrintInfo;

	//和连接池有关的
	std::list<lpngx_connection_t>  m_connectionList;     //总连接列表[连接池]
	static int  m_total_connection_n; //连接池总连接数

	std::list<lpngx_connection_t>  m_freeconnectionList; //空闲连接列表
	static int  m_free_connection_n;  //连接池空闲连接数
	pthread_mutex_t  freeConnListMutex;  //空闲连接队列相关互斥量

	//MESSAGE_QUEUE2 freeConnList;
	//pthread_mutex_t  freeConnListMutex;  //空闲连接队列相关互斥量，
	//std::list<lpngx_connection_t>  m_recyconnectionList; //将要释放的连接
	//int  m_recycling_connection_n; //待释放连接队列大小
	//pthread_mutex_t  recyConnMutex; //回收连接互斥量
									 //互斥m_recyconnectionList

	//监听套接字队列
	std::vector<lpngx_listening_t> m_ListenSocketList;

	//消息队列
	//=========================================================================
	std::list<char*>	m_MsgRecvQueue;		      //接收数据消息队列
	int                 m_iRecvMsgQueueCount;     //收消息队列大小

	std::list<char*>    m_MsgSendQueue;           //发送数据消息队列
	int                 m_iSendMsgQueueCount;     //发消息队列大小

	//多线程相关
	std::vector<ThreadItem*> m_threadVector;   //辅助线程容器	
	pthread_mutex_t   m_sendMessageQueueMutex; //发消息队列互斥量 
	pthread_mutex_t   m_recvMessageQueueMutex; //收消息队列互斥量

	static std::atomic<int>  onlineUserCount; //当前在线用户数统计相关

protected:
	//=========================================================================
	//所有配置相关的变量
	static int  m_ifkickTimeCount; //是否开启踢人时钟，1：开启，0：不开启
	static int  m_ifTimeOutKick; //当时间到达Sock_MaxWaitTime指定的时间时，直接把客户端踢出去；
								 //只有当Sock_WaitTimeEnable=1时，本项才有用
	static int  m_iWaitTime;     //多少秒检测一次心跳超时，
								 //只有当Sock_WaitTimeEnable=1时，本项才有用
	static int  m_RecyConnWaitTime; //回收连接等待时间
	static int  m_worker_connections; //epoll连接的最大项数
	static int	m_ListenPortCount; //所监听的端口数量
	static int  IOThreadCount; //IO处理线程池线程数量
	static int  connBuffSize; //每个连接所分配的收消息缓冲区大小
	//============================================================================================= 

public: //一些消息队列和连接队列
	int  epollHandleListen; //epoll_create返回的句柄
	struct epoll_event  m_events[NGX_MAX_EVENTS]; //用于在epoll_wait()中承载返回的所发生的事件的数组

	static MESSAGE_QUEUE  recvMsgQueue; //must be atomic locked when handling
	static MESSAGE_QUEUE  sendMsgQueue; //must be atomic locked when handling
	static MESSAGE_QUEUE  sendingQueue; //no need for atomic lock while only 
									   //handled by send thread

	static int        recvLOCK;     //atomic lock variable
	static int        sendLOCK;		//atomic lock variable	
	//volatile sem_t    speedUp[5];

	static MESSAGE_QUEUE2 recyConnQueue; //must be atomic locked when handling
	static int           connLOCK;		 //atomic lock variable

	static sem_t	   semRecyConnQueue; //the semaphore for handling recyConnQueue
	static sem_t       semEventSendQueue; //the semaphore for handling sendMsgQueue
	//=============================================================================================
};


//epoll_event结构体一般用在epoll机制中，其定义如下：
/*
struct epoll_event
{
  uint32_t events;      //Epoll events 
  epoll_data_t data;    //User data variable
} __attribute__ ((__packed__));

typedef union epoll_data
{
  void  * ptr;
  int fd;
  uint32_t u32;
  uint64_t u64;
} epoll_data_t;
*/

#endif
