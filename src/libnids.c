/*
  Copyright (c) 1999 Rafal Wojtczuk <nergal@7bulls.com>. All rights reserved.
  See the file COPYING for license details.
*/

#include<pthread.h>
#include <config.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <alloca.h>
#include <pcap.h>
#include <errno.h>
#include <config.h>

//#if (HAVE_UNISTD_H)
#include <unistd.h>
#include <stdio.h>
//#endif

#include "checksum.h"
#include "ip_fragment.h"
#include "scan.h"
#include "tcp.h"
#include "util.h"
#include "nids.h"

// add: 2014-2-22
// B-Queue
//#define CONS_BATCH
//#define PROD_BATCH
#include "fifo.h"
#define TEST_SIZE 20000
// add: 2014-3-21
//struct queue_t *fifo_queue;
#define FIFO_NUM 3
struct queue_t *fifo_queue[FIFO_NUM];
//
ELEMENT_TYPE inputelement;
//ELEMENT_TYPE outputelement;
static unsigned long inputcount;
static unsigned long outputcount;
static long discardcount;

// end add


#ifdef HAVE_LIBGTHREAD_2_0
#include <glib.h>
#endif

#ifdef __linux__
extern int set_all_promisc();
#endif

#define int_ntoa(x)	inet_ntoa(*((struct in_addr *)&x))

#define FIFO_MAX 32

//add: 2014 1 25
//#define _GNU_SOURCE

//#include<string.h>
//#include<sys/sem.h>
#include<semaphore.h>
//#include<sys/types.h>
//#include<sys/sysinfo.h>
//#include<sched.h>
//#include<ctype.h>
//#include<sys/syscall.h>
//end add

//newadd 2014 2 18

int coreNum=0;

//end newadd


extern int ip_options_compile(unsigned char *);
extern int raw_init();

static void nids_syslog(int, int, struct ip *, void *);
static int nids_ip_filter(struct ip *, int);

static struct proc_node *ip_frag_procs;
static struct proc_node *ip_procs;
static struct proc_node *udp_procs;

struct proc_node *tcp_procs;
static int linktype;
static pcap_t *desc = NULL;

// modified
static struct nids_fifo *fifo;
static int tcp_put = 0;
static int tcp_get = 0;
// modified 2014-01-26


#ifdef HAVE_LIBGTHREAD_2_0

/* async queue for multiprocessing - mcree */
static GAsyncQueue *cap_queue;

/* items in the queue */
struct cap_queue_item
{
	void *data;
	bpf_u_int32 caplen;
};

/* marks end of queue */
static struct cap_queue_item EOF_item;

/* error buffer for glib calls */
static GError *gerror = NULL;

#endif

char nids_errbuf[PCAP_ERRBUF_SIZE];
/// pcap_pkthdr是一个数据链路层帧头结构
// 参考:http://blog.csdn.net/yaneng/article/details/4315516
// 或者:http://blog.sina.com.cn/s/blog_94d26ea60100w3kt.html
struct pcap_pkthdr * nids_last_pcap_header = NULL;
// 指向最新的pcap包
u_char *nids_last_pcap_data = NULL;
u_int nids_linkoffset = 0;

char *nids_warnings[] =
{
	"Murphy - you never should see this message !",
	"Oversized IP packet",
	"Invalid IP fragment list: fragment over size",
	"Overlapping IP fragments",
	"Invalid IP header",
	"Source routed IP frame",
	"Max number of TCP streams reached",
	"Invalid TCP header",
	"Too much data in TCP receive queue",
	"Invalid TCP flags"
};

// 这里定义一个 nids_params变量，其他文件也是可见的。
struct nids_prm nids_params =
{
	1040,			/* n_tcp_streams */
	256,			/* n_hosts */
	NULL,			/* device */
	"tracefile.pcap",			/* filename */
	168,			/* sk_buff_size */
	-1,				/* dev_addon */
	nids_syslog,		/* syslog() */
	LOG_ALERT,			/* syslog_level */
	256,			/* scan_num_hosts */
	3000,			/* scan_delay */
	10,				/* scan_num_ports */
	nids_no_mem,		/* no_mem() */
	nids_ip_filter,		/* ip_filter() */
	NULL,			/* pcap_filter */
	1,				/* promisc */
	0,				/* one_loop_less */
	1024,			/* pcap_timeout */
	0,				/* multiproc */
	20000,			/* queue_limit */
	0,				/* tcp_workarounds */
	NULL,			/* pcap_desc */
	0,			        /* tcp_flow_timeout */
};





// 传入一个ip数据结构，传入一个ip分组的长度
// 这一个函数没有实现任何功能
// 总是返回1
static int nids_ip_filter(struct ip *x, int len)
{
	(void)x;
	(void)len;
	return 1;
}

// 这是一个日志管理函数
static void nids_syslog(int type, int errnum, struct ip *iph, void *data)
{
	char saddr[20], daddr[20];
	char buf[1024];
	struct host *this_host;
	unsigned char flagsand = 255, flagsor = 0;
	int i;

	switch (type)
	{

	case NIDS_WARN_IP:
		if (errnum != NIDS_WARN_IP_HDR)
		{
			strcpy(saddr, int_ntoa(iph->ip_src.s_addr));
			strcpy(daddr, int_ntoa(iph->ip_dst.s_addr));
			syslog(nids_params.syslog_level,
			       "%s, packet (apparently) from %s to %s\n",
			       nids_warnings[errnum], saddr, daddr);
		}
		else
			syslog(nids_params.syslog_level, "%s\n",
			       nids_warnings[errnum]);
		break;

	case NIDS_WARN_TCP:
		strcpy(saddr, int_ntoa(iph->ip_src.s_addr));
		strcpy(daddr, int_ntoa(iph->ip_dst.s_addr));
		if (errnum != NIDS_WARN_TCP_HDR)
			syslog(nids_params.syslog_level,
			       "%s,from %s:%hu to  %s:%hu\n", nids_warnings[errnum],
			       saddr, ntohs(((struct tcphdr *) data)->th_sport), daddr,
			       ntohs(((struct tcphdr *) data)->th_dport));
		else
			syslog(nids_params.syslog_level, "%s,from %s to %s\n",
			       nids_warnings[errnum], saddr, daddr);
		break;

	case NIDS_WARN_SCAN:
		this_host = (struct host *) data;
		sprintf(buf, "Scan from %s. Scanned ports: ",
		        int_ntoa(this_host->addr));
		for (i = 0; i < this_host->n_packets; i++)
		{
			strcat(buf, int_ntoa(this_host->packets[i].addr));
			sprintf(buf + strlen(buf), ":%hu,",
			        this_host->packets[i].port);
			flagsand &= this_host->packets[i].flags;
			flagsor |= this_host->packets[i].flags;
		}
		if (flagsand == flagsor)
		{
			i = flagsand;
			switch (flagsand)
			{
			case 2:
				strcat(buf, "scan type: SYN");
				break;
			case 0:
				strcat(buf, "scan type: NULL");
				break;
			case 1:
				strcat(buf, "scan type: FIN");
				break;
			default:
				sprintf(buf + strlen(buf), "flags=0x%x", i);
			}
		}
		else
			strcat(buf, "various flags");
		syslog(nids_params.syslog_level, "%s", buf);
		break;

	default:
		syslog(nids_params.syslog_level, "Unknown warning number ?\n");
	}
}


/* called either directly from pcap_hand() or from cap_queue_process_thread()
 * depending on the value of nids_params.multiproc - mcree
 */

static void call_ip_frag_procs(void *data,bpf_u_int32 caplen)
{
	struct proc_node *i;
	for (i = ip_frag_procs; i; i = i->next)
		(i->item) (data, caplen);
}


/* wireless frame types, mostly from tcpdump (wam) */
#define FC_TYPE(fc)             (((fc) >> 2) & 0x3)
#define FC_SUBTYPE(fc)          (((fc) >> 4) & 0xF)
#define DATA_FRAME_IS_QOS(x)    ((x) & 0x08)
#define FC_WEP(fc)              ((fc) & 0x4000)
#define FC_TO_DS(fc)            ((fc) & 0x0100)
#define FC_FROM_DS(fc)          ((fc) & 0x0200)
#define T_MGMT 0x0		/* management */
#define T_CTRL 0x1		/* control */
#define T_DATA 0x2		/* data */
#define T_RESV 0x3		/* reserved */
#define EXTRACT_LE_16BITS(p) \
	((unsigned short)*((const unsigned char *)(p) + 1) << 8 | \
	(unsigned short)*((const unsigned char *)(p) + 0))
#define EXTRACT_16BITS(p)	((unsigned short)ntohs(*(const unsigned short *)(p)))
#define LLC_FRAME_SIZE 8
#define LLC_OFFSET_TO_TYPE_FIELD 6
#define ETHERTYPE_IP 0x0800


// 这个函数应该是pcap接收到一个包之后，回调的函数
// 没有加static所以，是一个外部可见的函数
/*
	这一个函数，应该是pcap的回调函数，
	每当pcap抓到一个包之后，就会回调这个函数
	*/

/*
struct pcap_pkthdr
{
      struct timeval ts;   ts是一个结构struct timeval，它有两个部分，第一部分是1900开始以来的秒数，第二部分是当前秒之后的毫秒数
      bpf_u_int32 caplen;  表示抓到的数据长度
      bpf_u_int32 len;    表示数据包的实际长度
}
*/
void nids_pcap_handler(u_char * par, struct pcap_pkthdr *hdr, u_char * data)
{


	u_char *data_aligned;
#ifdef HAVE_LIBGTHREAD_2_0
	struct cap_queue_item *qitem;
#endif
#ifdef DLT_IEEE802_11
	unsigned short fc;
	int linkoffset_tweaked_by_prism_code = 0;
	int linkoffset_tweaked_by_radio_code = 0;
#endif

	/*
	 * Check for savagely closed TCP connections. Might
	 * happen only when nids_params.tcp_workarounds is non-zero;
	 * otherwise nids_tcp_timeouts is always NULL.
	 */
	// 首先检查是否有tcp超时
	if (NULL != nids_tcp_timeouts)
		tcp_check_timeouts(&hdr->ts);

// 将传进来的数据链路包头赋值给全局的最新pcap包头结构体指针
	// ip_fragment.c中的ip处理函数
	nids_last_pcap_header = hdr;
	// 这是pcap捕获的data，赋值给全局指针
	nids_last_pcap_data = data;
	// 这一个变量没有使用，以后扩展
	(void)par; /* warnings... */


	// 根据链接类型进行处理
	switch (linktype)
	{
		// 10MB
	case DLT_EN10MB:
		// 如果捕获的包长度<14 (14是数据链路包头大小)，那么不是一个完整的数据链路包
		// 参考: 2013年王道网络95页
		// 参考: http://blog.csdn.net/yaneng/article/details/4315516
		if (hdr->caplen < 14)
			return;

		/* Only handle IP packets and 802.1Q VLAN tagged packets below. */
		// 这两个字节正好是type字段
		// 参考: 2013年王道网络95页
		if (data[12] == 8 && data[13] == 0)
		{
			/* Regular ethernet */
			// 修改数据链路层的数据偏移，标准的以太网头大小是14B
			nids_linkoffset = 14;
		}
		// 参考:http://baike.baidu.com/link?url=vxhfREoPvIFmDDMvrGnsxEbOXbYmVDuD_kgColXq_gny7opbqII1M1b7-3hR1Vi1ORar1TcRi3XG9RxW0-PvVa#2
		else if (data[12] == 0x81 && data[13] == 0)
		{
			/* Skip 802.1Q VLAN and priority information */
			// 802.1q包头大小为18字节
			nids_linkoffset = 18;
		}
		else
			/* non-ip frame */
			return;
		break;

#ifdef DLT_PRISM_HEADER
#ifndef DLT_IEEE802_11
#error DLT_PRISM_HEADER is defined, but DLT_IEEE802_11 is not ???
#endif
	case DLT_PRISM_HEADER:
		//sizeof(prism2_hdr);
		nids_linkoffset = 144;
		linkoffset_tweaked_by_prism_code = 1;
		//now let DLT_IEEE802_11 do the rest
#endif
#ifdef DLT_IEEE802_11_RADIO
	case DLT_IEEE802_11_RADIO:
		// just get rid of the radio tap header
		if (!linkoffset_tweaked_by_prism_code)
		{
			nids_linkoffset = EXTRACT_LE_16BITS(data + 2); // skip radiotap header
			linkoffset_tweaked_by_radio_code = 1;
		}
		//now let DLT_IEEE802_11 do the rest
#endif
#ifdef DLT_IEEE802_11
	case DLT_IEEE802_11:
		/* I don't know why frame control is always little endian, but it
		 * works for tcpdump, so who am I to complain? (wam)
		 */
		if (!linkoffset_tweaked_by_prism_code && !linkoffset_tweaked_by_radio_code)
			nids_linkoffset = 0;
		fc = EXTRACT_LE_16BITS(data + nids_linkoffset);
		if (FC_TYPE(fc) != T_DATA || FC_WEP(fc))
		{
			return;
		}
		if (FC_TO_DS(fc) && FC_FROM_DS(fc))
		{
			/* a wireless distribution system packet will have another
			 * MAC addr in the frame
			 */
			nids_linkoffset += 30;
		}
		else
		{
			nids_linkoffset += 24;
		}
		if (DATA_FRAME_IS_QOS(FC_SUBTYPE(fc)))
			nids_linkoffset += 2;
		if (hdr->len < nids_linkoffset + LLC_FRAME_SIZE)
			return;
		if (ETHERTYPE_IP !=
		        EXTRACT_16BITS(data + nids_linkoffset + LLC_OFFSET_TO_TYPE_FIELD))
		{
			/* EAP, LEAP, and other 802.11 enhancements can be
			 * encapsulated within a data packet too.  Look only at
			 * encapsulated IP packets (Type field of the LLC frame).
			 */
			return;
		}
		nids_linkoffset += LLC_FRAME_SIZE;
		break;
#endif
	default:
		;
	}


	/*-------------------------------------------------------------
	至此，链接类型已经判断完毕，主要修改了 likeoffset这个全局变量
	-------------------------------------------------------------*/
	// 如果捕获的大小，比头还要小，那么显然是错误的，直接返回
	if (hdr->caplen < nids_linkoffset)
		return;
	// 否则继续往下执行，开始处理这个包--通常的手段就是保存下来


	/*
	* sure, memcpy costs. But many EXTRACT_{SHORT, LONG} macros cost, too.
	* Anyway, libpcap tries to ensure proper layer 3 alignment (look for
	* handle->offset in pcap sources), so memcpy should not be called.
	*/
#ifdef LBL_ALIGN
	// 如果是4的奇数倍，就执行下面的if
	if ((unsigned long) (data + nids_linkoffset) & 0x3)
	{
		data_aligned = alloca(hdr->caplen - nids_linkoffset + 4);
		data_aligned -= (unsigned long) data_aligned % 4;
		memcpy(data_aligned, data + nids_linkoffset, hdr->caplen - nids_linkoffset);
	}
	else
#endif
		// 如果没有定义上面的 预编译，那么
		// 无论如何都会执行下面这条语句
		// 如果定义而来上面的 预编译，那么
		// 只有在linkoffset为4的偶数倍的时候，才会执行下面这条语句
		data_aligned = data + nids_linkoffset;

#ifdef HAVE_LIBGTHREAD_2_0
	// 如果是多线程的
	if(nids_params.multiproc)
	{
		/*
		 * Insert received fragment into the async capture queue.
		 * We hope that the overhead of memcpy
		 * will be saturated by the benefits of SMP - mcree
		 */
		// 申请一块空间，将捕获的内容保存起来
		qitem=malloc(sizeof(struct cap_queue_item));
		// 如果申请成功，并且item的data也申请成功，则执行if
		if (qitem && (qitem->data=malloc(hdr->caplen - nids_linkoffset)))
		{
			// 记录item的长度(出去数据链路包头)
			qitem->caplen=hdr->caplen - nids_linkoffset;
			// 注意: data_aligned 是经过对齐了的数据
			// 拷贝数据链路包的内容，到item的data中
			memcpy(qitem->data,data_aligned,qitem->caplen);
			/*-------------------------------------------------------
			加锁准备处理queue中的内容
			---------------------------------------------------------*/
			/*********************************************************************************************/


			g_async_queue_lock(cap_queue);
			/* ensure queue does not overflow */
			// 如果大于队列的最大限制
			if(g_async_queue_length_unlocked(cap_queue) > nids_params.queue_limit)
			{
				/* queue limit reached: drop packet - should we notify user via syslog? */
				// 丢弃刚刚申请的内容
				// 可以优化的地方: 先判断，再申请，不要急着申请，然后释放
				// 但是可能加锁的地方会比较大，影响效率，这里先放着
				free(qitem->data);
				free(qitem);
			}
			else
			{
				/* insert packet to queue */
				// 加入队列
				g_async_queue_push_unlocked(cap_queue,qitem);
			}
			g_async_queue_unlock(cap_queue);
			/*-------------------------------------------------------
			处理完queue中的内容，解锁
			---------------------------------------------------------*/
		}
		// 如果申请失败，什么都不做
		// 我认为需要判断一下，是否应该释放qitem !!!
	}
	// 否则是用户要求单进程
	else     /* user requested simple passthru - no threading */
	{
		// 直接处理ip碎片
		call_ip_frag_procs(data_aligned,hdr->caplen - nids_linkoffset);
	}
#else
	// 否则直接就是单进程(线程)
	call_ip_frag_procs(data_aligned,hdr->caplen - nids_linkoffset);
#endif
}

/*
二层的PDU叫做Frame;
IP的叫做Packet;
TCP的叫做Segment；
UDP的叫做Datagram。
*/
// 生成IP 片段
// 这个函数会被nids_pcap_handler函数调用
// nids_pcap_handler应该是一个pcap的回调函数，每当有一个数据链路层的包
// 被pcap捕获，就会回调nids_pcap_handler(这个函数的定义就在上面)
static void gen_ip_frag_proc(u_char * data, int len)
{
	struct proc_node *i;
	struct ip *iph = (struct ip *) data;
	int need_free = 0;
	int skblen;
	// 定义一个指向函数的指针
	void (*glibc_syslog_h_workaround)(int, int, struct ip *, void*)=
	    nids_params.syslog;

	if (!nids_params.ip_filter(iph, len))
		return;

	if (len < (int)sizeof(struct ip) || iph->ip_hl < 5 || iph->ip_v != 4 ||
	        ip_fast_csum((unsigned char *) iph, iph->ip_hl) != 0 ||
	        len < ntohs(iph->ip_len) || ntohs(iph->ip_len) < iph->ip_hl << 2)
	{
		glibc_syslog_h_workaround(NIDS_WARN_IP, NIDS_WARN_IP_HDR, iph, 0);
		return;
	}
	if (iph->ip_hl > 5 && ip_options_compile((unsigned char *)data))
	{
		glibc_syslog_h_workaround(NIDS_WARN_IP, NIDS_WARN_IP_SRR, iph, 0);
		return;
	}

	// ip重组
	// 在ip_defrag_stub中还会调用
	// 每当有一个数据链路层的包过来，都会经过这里，将这个数据链路层
	// 的包，组装成ip报文
	switch (ip_defrag_stub((struct ip *) data, &iph))
	{
		// 出错，返回
	case IPF_ISF:
		return;
		// 还没有组成一个完整的ip报文，需要更多的ip碎片
	case IPF_NOTF:
		need_free = 0;
		iph = (struct ip *) data;
		break;
		// 已经组成了一个完整的ip报文，可以释放空间了
	case IPF_NEW:
		need_free = 1;
		break;
	default:
		;
	}

	// ip包长度+16
	skblen = ntohs(iph->ip_len) + 16;
	// 如果不需要释放，那么继续修改当前skb的长度，把刚刚获得的包添加进来
	if (!need_free)
		skblen += nids_params.dev_addon;
	// 这是一个+15然后求摸的操作，mod 16
	skblen = (skblen + 15) & ~15;
	skblen += nids_params.sk_buff_size;

	// 循环调用所有已经被注册过了的，处理关于IP的函数
	for (i = ip_procs; i; i = i->next)
		(i->item) (iph, skblen);
	// 如果需要释放，那么回到用free函数
	///////////////////if (need_free)
	/////////////////////	free(iph);
}

#if HAVE_BSD_UDPHDR
#define UH_ULEN uh_ulen
#define UH_SPORT uh_sport
#define UH_DPORT uh_dport
#else
#define UH_ULEN len
#define UH_SPORT source
#define UH_DPORT dest
#endif


static void process_udp(char *data)
{
	struct proc_node *ipp = udp_procs;
	struct ip *iph = (struct ip *) data;
	struct udphdr *udph;
	struct tuple4 addr;
	int hlen = iph->ip_hl << 2;
	int len = ntohs(iph->ip_len);
	int ulen;
	if (len - hlen < (int)sizeof(struct udphdr))
		return;
	udph = (struct udphdr *) (data + hlen);
	ulen = ntohs(udph->UH_ULEN);
	if (len - hlen < ulen || ulen < (int)sizeof(struct udphdr))
		return;
	/* According to RFC768 a checksum of 0 is not an error (Sebastien Raveau) */

	// 这里进行了udp的checksum
	if (udph->uh_sum && my_udp_check
	        ((void *) udph, ulen, iph->ip_src.s_addr,
	         iph->ip_dst.s_addr)) return;
// 经过了上面的check检查，如果没有问题，就会执行下面这些语句
	addr.source = ntohs(udph->UH_SPORT);
	addr.dest = ntohs(udph->UH_DPORT);
	addr.saddr = iph->ip_src.s_addr;
	addr.daddr = iph->ip_dst.s_addr;

	// 这个和上面一个函数的for循环是等价的
	// 遍历所有已经注册的upd处理函数，然后进行处理
	while (ipp)
	{
		ipp->item(&addr, ((char *) udph) + sizeof(struct udphdr),
		          ulen - sizeof(struct udphdr), data);
		ipp = ipp->next;
	}
}


// modified
// This is mostly like consumer.
static void nids_function(int fifo_index)
{

	ELEMENT_TYPE current;

	while(1)
	{

		// The only thing we need to do is dequeue
		if (SUCCESS == dequeue(fifo_queue[fifo_index], &current))
		{
			process_tcp((u_char*)(current.data), current.skblen,fifo_index);
			printf("fifo%d out\n",fifo_index);
			// release this element
			// FIXME: I think it would be batter to release element
			// after process_tcp otherwise producer would risk reusing
			// this buffer before it is read for the next tcp datagram.
			//outputcount ++;
			//printf("\ntcp dequeue! output = %d\n", outputcount);
		}
		// buffer is empty
		else
		{
			// do nothing.
			while(SUCCESS != dequeue(fifo_queue[fifo_index], &current))
			{
				//sleep(1);
			}
			//printf("\nqueue is empty! \n");
		}
	}
}
// end - 2014-01-25



// 最终ip分组生成函数
// modified 2014-01-25


//copy from xuezhang
int fifo_hash(u_char * data)
{
	struct ip *this_iphdr = (struct ip *)data;
	struct tcphdr *this_tcphdr = (struct tcphdr *)(data + 4 * this_iphdr->ip_hl);
	u_int16_t temp[6],hash;
	int *ptr;
	int i;
	ptr=temp;
	*ptr++=this_iphdr->ip_src.s_addr;
	*ptr++=this_iphdr->ip_dst.s_addr;
	temp[4]=this_tcphdr->th_sport;
	temp[5]=this_tcphdr->th_dport;
	for(i=1,hash=temp[0]; i<6; i++)
	{
		hash^=temp[i];
	}
	return ((int)hash)%(FIFO_NUM);
}

static void gen_ip_proc(u_char * data, int skblen)
{
	struct ip *iph;
	signed int temp;
	iph = (struct ip *) data;
	//add:2014-3-21
	int hash_i=fifo_hash(data);
	//end add
	switch (iph->ip_p)
	{
		// 如果上层是TCP那么就调用TCP处理函数
	case IPPROTO_TCP:
		// Actually, this procedure will be called loopedly
		// so we don't need a while here.



		if (SUCCESS == enqueue(fifo_queue[hash_i], (char*)iph, skblen))
		{
			// it means buffer is not totally full yet if success
			//	inputcount++;
			//printf("\ntcp enqueue!, input = %d\n", inputcount);
			         printf("fifo%d in\n",hash_i);
				//sleep(2);
		}	
		else
		{
			// wait untill equeue successfully
			while(SUCCESS != enqueue(fifo_queue[hash_i], (char*)iph, skblen))
			{

				//sleep(1);
			}
			//printf("%d discarded!\n", discardcount);
			//discardcount ++;

		}

		break;
		// 如果上层是UDP那么就调用UDP处理函数
	case IPPROTO_UDP:
		process_udp((char *)data);
		break;
		// 如果是ICMP ...
	case IPPROTO_ICMP:
		if (nids_params.n_tcp_streams)
			process_icmp(data);
		break;

	default:
		break;
	}
}


// 初始化所有的注册处理函数
static void init_procs()
{

	ip_frag_procs = mknew(struct proc_node);

	ip_frag_procs->item = gen_ip_frag_proc;

	ip_frag_procs->next = 0;


	ip_procs = mknew(struct proc_node);
	ip_procs->item = gen_ip_proc;
	ip_procs->next = 0;


	tcp_procs = 0;

	udp_procs = 0;
}


void nids_register_udp(void (*x))
{
	register_callback(&udp_procs, x);
}

void nids_unregister_udp(void (*x))
{
	unregister_callback(&udp_procs, x);
}

void nids_register_ip(void (*x))
{
	register_callback(&ip_procs, x);
}

void nids_unregister_ip(void (*x))
{
	unregister_callback(&ip_procs, x);
}

void nids_register_ip_frag(void (*x))
{
	register_callback(&ip_frag_procs, x);
}

void nids_unregister_ip_frag(void (*x))
{
	unregister_callback(&ip_frag_procs, x);
}

static int open_live()
{
	char *device;
	int promisc = 0;

	if (nids_params.device == NULL)
		nids_params.device = pcap_lookupdev(nids_errbuf);
	if (nids_params.device == NULL)
		return 0;

	device = nids_params.device;
	if (!strcmp(device, "all"))
		device = "any";
	else
		promisc = (nids_params.promisc != 0);

	if ((desc = pcap_open_live(device, 16384, promisc,
	                           nids_params.pcap_timeout, nids_errbuf)) == NULL)
		return 0;
#ifdef __linux__
	if (!strcmp(device, "any") && nids_params.promisc
	        && !set_all_promisc())
	{
		nids_errbuf[0] = 0;
		strncat(nids_errbuf, strerror(errno), sizeof(nids_errbuf) - 1);
		return 0;
	}
#endif
	if (!raw_init())
	{
		nids_errbuf[0] = 0;
		strncat(nids_errbuf, strerror(errno), sizeof(nids_errbuf) - 1);
		return 0;
	}
	return 1;
}

#ifdef HAVE_LIBGTHREAD_2_0

#define START_CAP_QUEUE_PROCESS_THREAD() \
    if(nids_params.multiproc) { /* threading... */ \
	 if(!(g_thread_create_full((GThreadFunc)cap_queue_process_thread,NULL,0,FALSE,TRUE,G_THREAD_PRIORITY_LOW,&gerror))) { \
	    strcpy(nids_errbuf, "thread: "); \
	    strncat(nids_errbuf, gerror->message, sizeof(nids_errbuf) - 8); \
	    return 0; \
	 }; \
    }

#define STOP_CAP_QUEUE_PROCESS_THREAD() \
    if(nids_params.multiproc) { /* stop the capture process thread */ \
	 g_async_queue_push(cap_queue,&EOF_item); \
    }


/* thread entry point
 * pops capture queue items and feeds them to
 * the ip fragment processors - mcree
 */
// 这个函数将会是某一个thread的入口点，
// 这个函数完成的功能是，获取queue中的items然后把这些items送给碎片处理者
static void cap_queue_process_thread()
{
	struct cap_queue_item *qitem;

	while(1)   /* loop "forever" */
	{
		// 使用了一个锁机制，保证了从cap_queue中获取正确的数据
		qitem=g_async_queue_pop(cap_queue);

		// 如果结束了，那么久退出循环
		if (qitem==&EOF_item) break; /* EOF item received: we should exit */

		// 否则首先被调用的是 call_ip_frag_procs，
		// call_ip_frag_procs是循环的调用 ip_frag_procs链表中的所有函数，
		// ip_frag_procs链表的最后一项是 gen_ip_frag_proc函数
		// 用户添加的自定义函数，都会从ip_frag_procs链表头加入
		// gen_ip_frag_proc函数是frag处理的最后一环，所以在gen_ip_frag_proc函数中会循环调用ip_procs链表
		// ip_procs链表的最后一个节点是gen_ip_proc函数
		// 用户自定义的都是添加在ip_procs链表头
		// gen_ip_proc函数会根据情况调用上层的process_tcp process_udp process_icmp等函数
		// 例如调用了process_udp函数。
		// process_udp函数会循环遍历udp_procs链表中的所有处理udp的被用户注册了的函数
		call_ip_frag_procs(qitem->data,qitem->caplen);

		// 上面的函数执行完了之后，就可以释放空间了，然后执行下一个while
		free(qitem->data);
		free(qitem);
	}
	// 退出后杀死线程
	g_thread_exit(NULL);
}

#else

#define START_CAP_QUEUE_PROCESS_THREAD()
#define STOP_CAP_QUEUE_PROCESS_THREAD()

#endif


// 这里就是nids的全局初始化函数
int nids_init()
{

	ELEMENT_TYPE bq_node;
	ELEMENT_TYPE_P bq_current, bq_end;
	char * ptr;
	int fifo_i;
	///////////////////////////
	printf("\n nids_init 001 \n");


	/* free resources that previous usages might have allocated */
	nids_exit();

	if (nids_params.pcap_desc)
		desc = nids_params.pcap_desc;
	else if (nids_params.filename)
	{
		if ((desc = pcap_open_offline(nids_params.filename,
		                              nids_errbuf)) == NULL)
			return 0;
	}
	else if (!open_live())
		return 0;

	if (nids_params.pcap_filter != NULL)
	{
		u_int mask = 0;
		struct bpf_program fcode;

		if (pcap_compile(desc, &fcode, nids_params.pcap_filter, 1, mask) <
		        0) return 0;
		if (pcap_setfilter(desc, &fcode) == -1)
			return 0;
	}
	switch ((linktype = pcap_datalink(desc)))
	{
#ifdef DLT_IEEE802_11
#ifdef DLT_PRISM_HEADER
	case DLT_PRISM_HEADER:
#endif
#ifdef DLT_IEEE802_11_RADIO
	case DLT_IEEE802_11_RADIO:
#endif
	case DLT_IEEE802_11:
		/* wireless, need to calculate offset per frame */
		break;
#endif

#ifdef DLT_NULL
	case DLT_NULL:
		nids_linkoffset = 4;
		break;
#endif
	case DLT_EN10MB:
		nids_linkoffset = 14;
		break;
	case DLT_PPP:
		nids_linkoffset = 4;
		break;
		/* Token Ring Support by vacuum@technotronic.com, thanks dugsong! */
	case DLT_IEEE802:
		nids_linkoffset = 22;
		break;

	case DLT_RAW:
	case DLT_SLIP:
		nids_linkoffset = 0;
		break;
#define DLT_LINUX_SLL   113
	case DLT_LINUX_SLL:
		nids_linkoffset = 16;
		break;
#ifdef DLT_FDDI
	case DLT_FDDI:
		nids_linkoffset = 21;
		break;
#endif
#ifdef DLT_PPP_SERIAL
	case DLT_PPP_SERIAL:
		nids_linkoffset = 4;
		break;
#endif
	default:
		strcpy(nids_errbuf, "link type unknown");
		return 0;
	}
	if (nids_params.dev_addon == -1)
	{
		if (linktype == DLT_EN10MB)
			nids_params.dev_addon = 16;
		else
			nids_params.dev_addon = 0;
	}
	if (nids_params.syslog == nids_syslog)
		openlog("libnids", 0, LOG_LOCAL0);
	// 注册所有函数
	init_procs();
	tcp_init(nids_params.n_tcp_streams);
	ip_frag_init(nids_params.n_hosts);
	scan_init();

	if(nids_params.multiproc)
	{
#ifdef HAVE_LIBGTHREAD_2_0
		g_thread_init(NULL);
		cap_queue=g_async_queue_new();
#else
		strcpy(nids_errbuf, "libnids was compiled without threads support");
		return 0;
#endif
	}



	///////////////////////////
	printf("\n nids_init 002 \n");

	// init
	for(fifo_i=0; fifo_i<FIFO_NUM; fifo_i++)
	{
		fifo_queue[fifo_i] = mknew(struct queue_t);
		if (!fifo_queue[fifo_i])
		{
			return 0;
		}
		// set fifo_queue with '0'

		queue_init(fifo_queue[fifo_i]);


		/////////////////////////////
		//inputcount = 0;
		//outputcount = 0;
		//discardcount = 0;

		///////////////////////////
		printf("\n nids_init 003 \n");

#if 0
		// init element for fifo_queue->data
		fifo_node_p = mknew_n(struct fifo_node, QUEUE_SIZE);
		if (!fifo_node_p)
		{
			free(fifo_queue);
			return 0;
		}
		buf_end = fifo_queue->data + QUEUE_SIZE;
		for (buf_current = fifo_queue->data; buf_current < buf_end; buf_current++, fifo_node_p ++)
		{
			(*buf_current) = fifo_node_p;
		}
#endif

		// allocate a buffer for tcp data.
		// 65535B for each tcp datagram.
		ptr = mknew_n(char, 65535*QUEUE_SIZE);
		if (!ptr)
		{
			free(fifo_queue[fifo_i]);
			return 0;
		}

		///////////////////////////
		printf("\n nids_init 004 \n");

		// Initialize fifo_node
		// Eache node has a point, 'data', pointing to a buffer.
		// There are 65535 buffers but they are allocated at a time
		// referenced by 'ptr'.
		bq_end = fifo_queue[fifo_i]->data + QUEUE_SIZE;
		for (bq_current = fifo_queue[fifo_i]->data; bq_current < bq_end;
		        bq_current++, ptr += 65535)
		{
			////////////////////////////////////
			//printf("bq_end=0x%p, data=0x%p, bq_current=0x%p \n", bq_end, fifo_queue->data, bq_current);
			bq_current->data = ptr;
			bq_current->skblen = -1;
		}

	}
	///////////////////////////
	printf("\n nids_init 005 \n");

	return 1;
}

int nids_run()
{


	// 如果pcat_t 的一个指针为空，则输出错误
	if (!desc)
	{
		strcpy(nids_errbuf, "Libnids not initialized");
		return 0;
	}


	//add: thread 2014 1 25   3
	FifoProces();
	//end add


	//START_CAP_QUEUE_PROCESS_THREAD(); /* threading... */

	//pcap_loop(desc, -1, (pcap_handler) nids_pcap_handler, 0);
	/* FIXME: will this code ever be called? Don't think so - mcree */
	// I don't think this code will ever be called, either.
	//STOP_CAP_QUEUE_PROCESS_THREAD();


	nids_exit();
	return 0;
}


//newadd 2014 2 18
void FifoProces()
{
	coreNum=sysconf(_SC_NPROCESSORS_CONF);//获取cpu数目
	//printf("core num=%d\n",coreNum);
	//tid 脫脙脌沤卤锚脢鸥虏禄脥卢碌脛脧脽鲁脤id潞脜拢卢脫脙脪脭掳贸露拧露脭脫脙碌脛cpu
	int i,tid[2]= {0,1};
	//thread_error=pthread_create(&th1,NULL,thread_pros1,(void*)&tid[0]);
	thread_error=pthread_create(&th_catch,NULL,thCatch,NULL);
	if(thread_error!=0)
	{
		sprintf("error:%s\n",strerror(thread_error));
		return 0;
	}
	//thread_error=pthread_create(&th2,NULL,thread_pros2,(void*)&tid[1]);
	thread_error=pthread_create(&th_pro1,NULL,thPro1,NULL);
	if(thread_error!=0)
	{
		sprintf("error:%s\n",strerror(thread_error));
		return 0;
	}
	thread_error=pthread_create(&th_pro2,NULL,thPro2,NULL);
	if(thread_error!=0)
	{
		sprintf("error:%s\n",strerror(thread_error));
		return 0;
	}
	thread_error=pthread_create(&th_pro3,NULL,thPro3,NULL);
	if(thread_error!=0)
	{
		sprintf("error:%s\n",strerror(thread_error));
		return 0;
	}
	pthread_join(th_catch,NULL);
	pthread_join(th_pro1,NULL);
	pthread_join(th_pro2,NULL);
	pthread_join(th_pro3,NULL);

}

//end newadd

//add: 2014 1 25 4
// running pcap_loop to capture packages from ethernet.
void * thCatch(void *arg)
{
	cpu_set_t mask;
	cpu_set_t get;
	printf("This is the first phrase!\n");
	//int *ar=(int *)arg;
	//int *ar=NULL;//debug
	//*ar=0;///debug
	//printf("this is the %d thread\n",*ar);

	CPU_ZERO(&mask);
	CPU_SET(0,&mask);
	if(-1==sched_setaffinity(0,sizeof(mask),&mask))
		printf("Faild to band %d thread on cpu\n",0);
	//else printf("%d thread band to %d cpu successfully\n",*ar,sched_getcpu());
	CPU_ZERO(&get);
	if(sched_getaffinity(0,sizeof(get),&get)<0)
		printf("faild get cpu source\n");


	//////////////////////////////////
	coreNum=sysconf(_SC_NPROCESSORS_CONF);
	printf("core num=%d\n",coreNum);
	printf("This is the first phrase!\n");
	printf("\"pcap_get\" thread is run on %d cpu\n",sched_getcpu());
        sleep(2);
	//////////////////////////////////

	pcap_loop(desc, -1, (pcap_handler) nids_pcap_handler, 0);
	STOP_CAP_QUEUE_PROCESS_THREAD();

	//todo :
	//gen_ip_frag_proc;
}

// running nids_functions to trip into a dead loop
// in the loop we tackle tcp datagrams.
void *thPro1(void * arg)
{
	cpu_set_t mask;//
	cpu_set_t get;//
	printf("This is the second phrase!\n");
	int fifo_index=0;
	//newadd 2014 2 18
	//int *ar=(int *)arg;
	//int *ar=NULL;
	//*ar=1;//debug
	//printf("this is the %d thread\n",*ar);

	CPU_ZERO(&mask);
	CPU_SET(1,&mask);
	if(-1==sched_setaffinity(0,sizeof(mask),&mask))
		printf("Faild to band %d thread on cpu\n",1);
	CPU_ZERO(&get);
	if(sched_getaffinity(0,sizeof(get),&get)<0)
		printf("faild get cpu source\n");


	//////////////////////////////////
	coreNum=sysconf(_SC_NPROCESSORS_CONF);
	printf("core num=%d\n",coreNum);

	sleep(2);
	//////////////////////////////////
	//else printf("%d thread band to %d cpu successfully\n",*ar,sched_getcpu());
	//end newadd
	nids_function(fifo_index);
	//todo :
	//tcp_procs;
}


void *thPro2(void * arg)
{
	cpu_set_t mask;//
	cpu_set_t get;//
	int fifo_index=1;
	printf("This is the third phrase!\n");
	//newadd 2014 2 18
	//int *ar=(int *)arg;
	//int *ar=NULL;
	//*ar=1;//debug
	//printf("this is the %d thread\n",*ar);

	CPU_ZERO(&mask);
	CPU_SET(2,&mask);
	if(-1==sched_setaffinity(0,sizeof(mask),&mask))
		printf("Faild to band %d thread on cpu\n",1);
	CPU_ZERO(&get);
	if(sched_getaffinity(0,sizeof(get),&get)<0)
		printf("faild get cpu source\n");


	//////////////////////////////////
	coreNum=sysconf(_SC_NPROCESSORS_CONF);
	printf("core num=%d\n",coreNum);

	sleep(2);
	//////////////////////////////////
	//else printf("%d thread band to %d cpu successfully\n",*ar,sched_getcpu());
	//end newadd
	///////////////////////////////////
	nids_function(fifo_index);
	//todo :
	//tcp_procs;
}


void *thPro3(void * arg)
{
	cpu_set_t mask;//
	cpu_set_t get;//
	int fifo_index=2;
	printf("This is the fourth phrase!\n");
	//newadd 2014 2 18
	//int *ar=(int *)arg;
	//int *ar=NULL;
	//*ar=1;//debug
	//printf("this is the %d thread\n",*ar);

	CPU_ZERO(&mask);
	CPU_SET(3,&mask);
	if(-1==sched_setaffinity(0,sizeof(mask),&mask))
		printf("Faild to band %d thread on cpu\n",1);
	CPU_ZERO(&get);
	if(sched_getaffinity(0,sizeof(get),&get)<0)
		printf("faild get cpu source\n");


	//////////////////////////////////
	coreNum=sysconf(_SC_NPROCESSORS_CONF);
	printf("core num=%d\n",coreNum);
	nids_function(fifo_index);
	sleep(2);
	//////////////////////////////////
	//else printf("%d thread band to %d cpu successfully\n",*ar,sched_getcpu());
	//end newadd
	//////////////////////////////////////////
	//nids_function();
	//todo :
	//tcp_procs;
}

//end add

void nids_exit()
{
	if (!desc)
	{
		strcpy(nids_errbuf, "Libnids not initialized");
		return;
	}
#ifdef HAVE_LIBGTHREAD_2_0
	if (nids_params.multiproc)
	{
		/* I have no portable sys_sched_yield,
		   and I don't want to add more synchronization...
		*/
		while (g_async_queue_length(cap_queue)>0)
			usleep(100000);
	}
#endif
	tcp_exit();
	ip_frag_exit();
	scan_exit();
	strcpy(nids_errbuf, "loop: ");
	strncat(nids_errbuf, pcap_geterr(desc), sizeof nids_errbuf - 7);
	if (!nids_params.pcap_desc)
		pcap_close(desc);
	desc = NULL;

	free(ip_procs);
	free(ip_frag_procs);
}

int nids_getfd()
{
	if (!desc)
	{
		strcpy(nids_errbuf, "Libnids not initialized");
		return -1;
	}
	return pcap_get_selectable_fd(desc);
}

int nids_next()
{
	struct pcap_pkthdr h;
	char *data;

	if (!desc)
	{
		strcpy(nids_errbuf, "Libnids not initialized");
		return 0;
	}
	if (!(data = (char *) pcap_next(desc, &h)))
	{
		strcpy(nids_errbuf, "next: ");
		strncat(nids_errbuf, pcap_geterr(desc), sizeof(nids_errbuf) - 7);
		return 0;
	}
	/* threading is quite useless (harmful) in this case - should we do an API change?  */
	START_CAP_QUEUE_PROCESS_THREAD();
	nids_pcap_handler(0, &h, (u_char *)data);
	STOP_CAP_QUEUE_PROCESS_THREAD();
	return 1;
}

int nids_dispatch(int cnt)
{
	int r;

	if (!desc)
	{
		strcpy(nids_errbuf, "Libnids not initialized");
		return -1;
	}
	START_CAP_QUEUE_PROCESS_THREAD(); /* threading... */
	if ((r = pcap_dispatch(desc, cnt, (pcap_handler) nids_pcap_handler,
	                       NULL)) == -1)
	{
		strcpy(nids_errbuf, "dispatch: ");
		strncat(nids_errbuf, pcap_geterr(desc), sizeof(nids_errbuf) - 11);
	}
	STOP_CAP_QUEUE_PROCESS_THREAD();
	return r;
}





