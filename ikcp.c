//=====================================================================
//
// KCP - A Better ARQ Protocol Implementation
// skywind3000 (at) gmail.com, 2010-2011
//  
// Features:
// + Average RTT reduce 30% - 40% vs traditional ARQ like tcp.
// + Maximum RTT reduce three times vs tcp.
// + Lightweight, distributed as a single source file.
//
//=====================================================================
#include "ikcp.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

//=====================================================================
// KCP BASIC
//=====================================================================
const IUINT32 IKCP_RTO_NDL = 30;		// no delay min rto
const IUINT32 IKCP_RTO_MIN = 100;		// normal min rto
const IUINT32 IKCP_RTO_DEF = 200;
const IUINT32 IKCP_RTO_MAX = 60000;
const IUINT32 IKCP_CMD_PUSH = 81;		// cmd: push data
const IUINT32 IKCP_CMD_ACK  = 82;		// cmd: ack
const IUINT32 IKCP_CMD_WASK = 83;		// cmd: window probe (ask)
const IUINT32 IKCP_CMD_WINS = 84;		// cmd: window size (tell)
const IUINT32 IKCP_ASK_SEND = 1;		// need to send IKCP_CMD_WASK
const IUINT32 IKCP_ASK_TELL = 2;		// need to send IKCP_CMD_WINS
const IUINT32 IKCP_WND_SND = 32;
const IUINT32 IKCP_WND_RCV = 32;
const IUINT32 IKCP_MTU_DEF = 1400;
const IUINT32 IKCP_ACK_FAST	= 3;
const IUINT32 IKCP_INTERVAL	= 100;
const IUINT32 IKCP_OVERHEAD = 24;
const IUINT32 IKCP_DEADLINK = 20;
const IUINT32 IKCP_THRESH_INIT = 2;
const IUINT32 IKCP_THRESH_MIN = 2;
const IUINT32 IKCP_PROBE_INIT = 7000;		// 7 secs to probe window size
const IUINT32 IKCP_PROBE_LIMIT = 120000;	// up to 120 secs to probe window


//---------------------------------------------------------------------
// encode / decode
//---------------------------------------------------------------------

/* encode 8 bits unsigned int */
static inline char *ikcp_encode8u(char *p, unsigned char c)
{
	*(unsigned char*)p++ = c;
	return p;
}

/* decode 8 bits unsigned int */
static inline const char *ikcp_decode8u(const char *p, unsigned char *c)
{
	*c = *(unsigned char*)p++;
	return p;
}

/* encode 16 bits unsigned int (lsb) */
static inline char *ikcp_encode16u(char *p, unsigned short w)
{
#if IWORDS_BIG_ENDIAN
	*(unsigned char*)(p + 0) = (w & 255);
	*(unsigned char*)(p + 1) = (w >> 8);
#else
	*(unsigned short*)(p) = w;
#endif
	p += 2;
	return p;
}

/* decode 16 bits unsigned int (lsb) */
static inline const char *ikcp_decode16u(const char *p, unsigned short *w)
{
#if IWORDS_BIG_ENDIAN
	*w = *(const unsigned char*)(p + 1);
	*w = *(const unsigned char*)(p + 0) + (*w << 8);
#else
	*w = *(const unsigned short*)p;
#endif
	p += 2;
	return p;
}

/* encode 32 bits unsigned int (lsb) */
static inline char *ikcp_encode32u(char *p, IUINT32 l)
{
#if IWORDS_BIG_ENDIAN
	*(unsigned char*)(p + 0) = (unsigned char)((l >>  0) & 0xff);
	*(unsigned char*)(p + 1) = (unsigned char)((l >>  8) & 0xff);
	*(unsigned char*)(p + 2) = (unsigned char)((l >> 16) & 0xff);
	*(unsigned char*)(p + 3) = (unsigned char)((l >> 24) & 0xff);
#else
	*(IUINT32*)p = l;
#endif
	p += 4;
	return p;
}

/* decode 32 bits unsigned int (lsb) */
static inline const char *ikcp_decode32u(const char *p, IUINT32 *l)
{
#if IWORDS_BIG_ENDIAN
	*l = *(const unsigned char*)(p + 3);
	*l = *(const unsigned char*)(p + 2) + (*l << 8);
	*l = *(const unsigned char*)(p + 1) + (*l << 8);
	*l = *(const unsigned char*)(p + 0) + (*l << 8);
#else 
	*l = *(const IUINT32*)p;
#endif
	p += 4;
	return p;
}

static inline IUINT32 _imin_(IUINT32 a, IUINT32 b) {
	return a <= b ? a : b;
}

static inline IUINT32 _imax_(IUINT32 a, IUINT32 b) {
	return a >= b ? a : b;
}

static inline IUINT32 _ibound_(IUINT32 lower, IUINT32 middle, IUINT32 upper) 
{
	return _imin_(_imax_(lower, middle), upper);
}

static inline long _itimediff(IUINT32 later, IUINT32 earlier) 
{
	return ((IINT32)(later - earlier));
}

//---------------------------------------------------------------------
// manage segment
//---------------------------------------------------------------------
typedef struct IKCPSEG IKCPSEG;

static void* (*ikcp_malloc_hook)(size_t) = NULL;
static void (*ikcp_free_hook)(void *) = NULL;

// internal malloc
static void* ikcp_malloc(size_t size) {
	if (ikcp_malloc_hook) 
		return ikcp_malloc_hook(size);
	return malloc(size);
}

// internal free
static void ikcp_free(void *ptr) {
	if (ikcp_free_hook) {
		ikcp_free_hook(ptr);
	}	else {
		free(ptr);
	}
}

// redefine allocator
void ikcp_allocator(void* (*new_malloc)(size_t), void (*new_free)(void*))
{
	ikcp_malloc_hook = new_malloc;
	ikcp_free_hook = new_free;
}

// allocate a new kcp segment
static IKCPSEG* ikcp_segment_new(ikcpcb *kcp, int size)
{
	return (IKCPSEG*)ikcp_malloc(sizeof(IKCPSEG) + size);
}

// delete a segment
static void ikcp_segment_delete(ikcpcb *kcp, IKCPSEG *seg)
{
	ikcp_free(seg);
}

// write log
void ikcp_log(ikcpcb *kcp, int mask, const char *fmt, ...)
{
	char buffer[1024];
	va_list argptr;
	if ((mask & kcp->logmask) == 0 || kcp->writelog == 0) return;
	va_start(argptr, fmt);
	vsprintf(buffer, fmt, argptr);
	va_end(argptr);
	kcp->writelog(buffer, kcp, kcp->user);
}

// check log mask
static int ikcp_canlog(const ikcpcb *kcp, int mask)
{
	if ((mask & kcp->logmask) == 0 || kcp->writelog == NULL) return 0;
	return 1;
}

// output segment
static int ikcp_output(ikcpcb *kcp, const void *data, int size)
{
	assert(kcp);
	assert(kcp->output);
	if (ikcp_canlog(kcp, IKCP_LOG_OUTPUT)) {
		ikcp_log(kcp, IKCP_LOG_OUTPUT, "[RO] %ld bytes", (long)size);
	}
	if (size == 0) return 0;
	return kcp->output((const char*)data, size, kcp, kcp->user);
}

// output queue
void ikcp_qprint(const char *name, const struct IQUEUEHEAD *head)
{
#if 0
	const struct IQUEUEHEAD *p;
	printf("<%s>: [", name);
	for (p = head->next; p != head; p = p->next) {
		const IKCPSEG *seg = iqueue_entry(p, const IKCPSEG, node);
		printf("(%lu %d)", (unsigned long)seg->sn, (int)(seg->ts % 10000));
		if (p->next != head) printf(",");
	}
	printf("]\n");
#endif
}


//---------------------------------------------------------------------
// create a new kcpcb
// ������Ҫ����һ��kcp���ڹ���������Ĺ������̣�
// �ڴ�����ʱ��Ĭ�ϵķ��͡������Լ�Զ�˵Ĵ��ڴ�С��Ϊ32��
// mtu��СΪ1400bytes��mssΪ1400-24=1376bytes��
// ��ʱ�ش�ʱ��Ϊ200���룬��С�ش�ʱ��Ϊ100���룬
// kcp�ڲ������Сʱ��Ϊ100����(kcp->interval = IKCP_INTERVAL;)��
// ����ط����� dead_link ΪIKCP_DEADLINK��20��
//---------------------------------------------------------------------
ikcpcb* ikcp_create(IUINT32 conv, void *user)
{
	ikcpcb *kcp = (ikcpcb*)ikcp_malloc(sizeof(struct IKCPCB));
	if (kcp == NULL) return NULL;
	kcp->conv = conv;
	kcp->user = user;
	kcp->snd_una = 0;
	kcp->snd_nxt = 0;
	kcp->rcv_nxt = 0;
	kcp->ts_recent = 0;
	kcp->ts_lastack = 0;
	kcp->ts_probe = 0;
	kcp->probe_wait = 0;
	kcp->snd_wnd = IKCP_WND_SND;
	kcp->rcv_wnd = IKCP_WND_RCV;
	kcp->rmt_wnd = IKCP_WND_RCV;
	kcp->cwnd = 0;
	kcp->incr = 0;
	kcp->probe = 0;
	kcp->mtu = IKCP_MTU_DEF;
	kcp->mss = kcp->mtu - IKCP_OVERHEAD;
	kcp->stream = 0;

	kcp->buffer = (char*)ikcp_malloc((kcp->mtu + IKCP_OVERHEAD) * 3);
	if (kcp->buffer == NULL) {
		ikcp_free(kcp);
		return NULL;
	}

	iqueue_init(&kcp->snd_queue);
	iqueue_init(&kcp->rcv_queue);
	iqueue_init(&kcp->snd_buf);
	iqueue_init(&kcp->rcv_buf);
	kcp->nrcv_buf = 0;
	kcp->nsnd_buf = 0;
	kcp->nrcv_que = 0;
	kcp->nsnd_que = 0;
	kcp->state = 0;
	kcp->acklist = NULL;
	kcp->ackblock = 0;
	kcp->ackcount = 0;
	kcp->rx_srtt = 0;
	kcp->rx_rttval = 0;
	kcp->rx_rto = IKCP_RTO_DEF;
	kcp->rx_minrto = IKCP_RTO_MIN;
	kcp->current = 0;
	kcp->interval = IKCP_INTERVAL;
	kcp->ts_flush = IKCP_INTERVAL;
	kcp->nodelay = 0;
	kcp->updated = 0;
	kcp->logmask = 0;
	kcp->ssthresh = IKCP_THRESH_INIT;
	kcp->fastresend = 0;
	kcp->nocwnd = 0;
	kcp->xmit = 0;
    kcp->dead_link = IKCP_DEADLINK;
	kcp->output = NULL;
	kcp->writelog = NULL;

	return kcp;
}


//---------------------------------------------------------------------
// release a new kcpcb
//---------------------------------------------------------------------
void ikcp_release(ikcpcb *kcp)
{
	assert(kcp);
	if (kcp) {
		IKCPSEG *seg;
		while (!iqueue_is_empty(&kcp->snd_buf)) {
			seg = iqueue_entry(kcp->snd_buf.next, IKCPSEG, node);
			iqueue_del(&seg->node);
			ikcp_segment_delete(kcp, seg);
		}
		while (!iqueue_is_empty(&kcp->rcv_buf)) {
			seg = iqueue_entry(kcp->rcv_buf.next, IKCPSEG, node);
			iqueue_del(&seg->node);
			ikcp_segment_delete(kcp, seg);
		}
		while (!iqueue_is_empty(&kcp->snd_queue)) {
			seg = iqueue_entry(kcp->snd_queue.next, IKCPSEG, node);
			iqueue_del(&seg->node);
			ikcp_segment_delete(kcp, seg);
		}
		while (!iqueue_is_empty(&kcp->rcv_queue)) {
			seg = iqueue_entry(kcp->rcv_queue.next, IKCPSEG, node);
			iqueue_del(&seg->node);
			ikcp_segment_delete(kcp, seg);
		}
		if (kcp->buffer) {
			ikcp_free(kcp->buffer);
		}
		if (kcp->acklist) {
			ikcp_free(kcp->acklist);
		}

		kcp->nrcv_buf = 0;
		kcp->nsnd_buf = 0;
		kcp->nrcv_que = 0;
		kcp->nsnd_que = 0;
		kcp->ackcount = 0;
		kcp->buffer = NULL;
		kcp->acklist = NULL;
		ikcp_free(kcp);
	}
}


//---------------------------------------------------------------------
// set output callback, which will be invoked by kcp
//---------------------------------------------------------------------
void ikcp_setoutput(ikcpcb *kcp, int (*output)(const char *buf, int len,
	ikcpcb *kcp, void *user))
{
	kcp->output = output;
}


//---------------------------------------------------------------------
// user/upper level recv: returns size, returns below zero for EAGAIN
//
// �ϲ����kcp��receive�������Ὣrcv_queue�е����ݷֶ����������buffer���û��������У�
// Ȼ��ɾ����Ӧ��Segment����������ת��ǰ���ȼ���һ�鱾�����ݰ����ܴ�С��
// ֻ�д�С����ʱ�Ż��û��Ż��յ����ݡ�
//
// Ȼ���ڽ��ջ�������Ѱ����һ����Ҫ���յ�Segment��
// ����ҵ��򽫸�Segmentת�Ƶ�rcv_queue�еȴ��´��û��ٵ���receive�������� ��
//
// ��Ҫע����ǣ�Segment�ڴ�bufת��queue��ʱ��ȷ��ת�Ƶ�Segment��sn��Ϊ�´���Ҫ���յģ�
// ���򽫲���ת�ƣ����queue�е�Segment��������ģ���buf�е�Segment���ܻ�������ġ�
//
// ֮������û��������ݺ�Ĵ��ڱ仯������Զ�˽��д��ڻָ���
//---------------------------------------------------------------------
int ikcp_recv(ikcpcb *kcp, char *buffer, int len)
{
	struct IQUEUEHEAD *p;
	int ispeek = (len < 0)? 1 : 0;
	int peeksize;
	int recover = 0;
	IKCPSEG *seg;
	assert(kcp);

	if (iqueue_is_empty(&kcp->rcv_queue))
		return -1;

	if (len < 0) len = -len;

	peeksize = ikcp_peeksize(kcp);

	if (peeksize < 0) 
		return -2;

	if (peeksize > len) 
		return -3;

	if (kcp->nrcv_que >= kcp->rcv_wnd)
		recover = 1;

	// merge fragment
	// ����rcv_queue��buffer
	for (len = 0, p = kcp->rcv_queue.next; p != &kcp->rcv_queue; ) {
		int fragment;
		seg = iqueue_entry(p, IKCPSEG, node);
		p = p->next;

		if (buffer) {
			memcpy(buffer, seg->data, seg->len);
			buffer += seg->len;
		}

		len += seg->len;
		fragment = seg->frg;

		if (ikcp_canlog(kcp, IKCP_LOG_RECV)) {
			ikcp_log(kcp, IKCP_LOG_RECV, "recv sn=%lu", seg->sn);
		}

		if (ispeek == 0) {
			iqueue_del(&seg->node);
			ikcp_segment_delete(kcp, seg);
			kcp->nrcv_que--;
		}

		if (fragment == 0) 
			break;
	}

	assert(len == peeksize);

	// move available data from rcv_buf -> rcv_queue
	while (! iqueue_is_empty(&kcp->rcv_buf)) {
		IKCPSEG *seg = iqueue_entry(kcp->rcv_buf.next, IKCPSEG, node);
		if (seg->sn == kcp->rcv_nxt && kcp->nrcv_que < kcp->rcv_wnd) {
			iqueue_del(&seg->node);
			kcp->nrcv_buf--;
			iqueue_add_tail(&seg->node, &kcp->rcv_queue);
			kcp->nrcv_que++;
			kcp->rcv_nxt++;
		}	else {
			break;
		}
	}

	// fast recover
	if (kcp->nrcv_que < kcp->rcv_wnd && recover) {
		// ready to send back IKCP_CMD_WINS in ikcp_flush
		// tell remote my window size
		kcp->probe |= IKCP_ASK_TELL;
	}

	return len;
}


//---------------------------------------------------------------------
// peek data size
//---------------------------------------------------------------------
int ikcp_peeksize(const ikcpcb *kcp)
{
	struct IQUEUEHEAD *p;
	IKCPSEG *seg;
	int length = 0;

	assert(kcp);

	if (iqueue_is_empty(&kcp->rcv_queue)) return -1;

	seg = iqueue_entry(kcp->rcv_queue.next, IKCPSEG, node);
	if (seg->frg == 0) return seg->len;

	if (kcp->nrcv_que < seg->frg + 1) return -1;

	for (p = kcp->rcv_queue.next; p != &kcp->rcv_queue; p = p->next) {
		seg = iqueue_entry(p, IKCPSEG, node);
		length += seg->len;
		if (seg->frg == 0) break;
	}

	return length;
}


//---------------------------------------------------------------------
// user/upper level send, returns below zero for error
// kcp���͵����ݰ���Ϊ2��ģʽ����ģʽ����ģʽ��
// 
// �ڰ�ģʽ�����ݰ����û����ε�send���ݷֽ磬��¼Segment��send_queue�У�
// ��������������mss��С�����з�Ƭ����
// ��Ƭ�ڵ�frg��¼��Ƭ��ţ��Ӵ�С��0���������ݵĽ�����
// 
// ����ģʽ�£�kcp�Ὣ�û�������ȫ��ƴ����һ��
// ��һ��send������Segment������пռ�ͽ������ݲ����ĩβ��
// ʣ�������ٴ����µ�Segment��send�Ĺ��̾��ǽ��û�����ת�Ƶ�Segment��
// Ȼ����ӵ����Ͷ����С�
//
// ��mssΪ���ݶ��û����ݷ�segment : 
// 
// - ��Ϣģʽ�����ݷ�Ƭ�������id�����η���snd_queue�����շ�����id���Ƭ���ݣ���Ƭ��С <= mss
// - ��ģʽ�������һ����Ƭ�Ƿ�ﵽmss����δ�ﵽ����䣬�����ʸ�һЩ
//---------------------------------------------------------------------
int ikcp_send(ikcpcb *kcp, const char *buffer, int len)
{
	IKCPSEG *seg;
	int count, i;

	assert(kcp->mss > 0);
	if (len < 0) return -1;

	// append to previous segment in streaming mode (if possible)
	if (kcp->stream != 0) {
		if (!iqueue_is_empty(&kcp->snd_queue)) {
			IKCPSEG *old = iqueue_entry(kcp->snd_queue.prev, IKCPSEG, node);
			if (old->len < kcp->mss) {
				int capacity = kcp->mss - old->len;
				int extend = (len < capacity)? len : capacity;
				seg = ikcp_segment_new(kcp, old->len + extend);
				assert(seg);
				if (seg == NULL) {
					return -2;
				}
				iqueue_add_tail(&seg->node, &kcp->snd_queue);
				memcpy(seg->data, old->data, old->len);
				if (buffer) {
					memcpy(seg->data + old->len, buffer, extend);
					buffer += extend;
				}
				seg->len = old->len + extend;
				seg->frg = 0;
				len -= extend;
				iqueue_del_init(&old->node);
				ikcp_segment_delete(kcp, old);
			}
		}
		if (len <= 0) {
			return 0;
		}
	}

	if (len <= (int)kcp->mss) count = 1;
	else count = (len + kcp->mss - 1) / kcp->mss;

	if (count > 255) return -2;

	if (count == 0) count = 1;

	// fragment
	for (i = 0; i < count; i++) {
		int size = len > (int)kcp->mss ? (int)kcp->mss : len;
		seg = ikcp_segment_new(kcp, size);
		assert(seg);
		if (seg == NULL) {
			return -2;
		}
		if (buffer && len > 0) {
			memcpy(seg->data, buffer, size);
		}
		seg->len = size;
		seg->frg = (kcp->stream == 0)? (count - i - 1) : 0;
		iqueue_init(&seg->node);
		iqueue_add_tail(&seg->node, &kcp->snd_queue);
		kcp->nsnd_que++;
		if (buffer) {
			buffer += size;
		}
		len -= size;
	}

	return 0;
}


//---------------------------------------------------------------------
// parse ack
//---------------------------------------------------------------------
static void ikcp_update_ack(ikcpcb *kcp, IINT32 rtt)
{
	IINT32 rto = 0;
	if (kcp->rx_srtt == 0) {
		kcp->rx_srtt = rtt;
		kcp->rx_rttval = rtt / 2;
	}	else {
		long delta = rtt - kcp->rx_srtt;
		if (delta < 0) delta = -delta;
		kcp->rx_rttval = (3 * kcp->rx_rttval + delta) / 4;
		kcp->rx_srtt = (7 * kcp->rx_srtt + rtt) / 8;
		if (kcp->rx_srtt < 1) kcp->rx_srtt = 1;
	}
	rto = kcp->rx_srtt + _imax_(kcp->interval, 4 * kcp->rx_rttval);
	kcp->rx_rto = _ibound_(kcp->rx_minrto, rto, IKCP_RTO_MAX);
}

static void ikcp_shrink_buf(ikcpcb *kcp)
{
	struct IQUEUEHEAD *p = kcp->snd_buf.next;
	if (p != &kcp->snd_buf) {
		IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
		kcp->snd_una = seg->sn;
	}	else {
		kcp->snd_una = kcp->snd_nxt;
	}
}

static void ikcp_parse_ack(ikcpcb *kcp, IUINT32 sn)
{
	struct IQUEUEHEAD *p, *next;

	// snС��snd_una����ڵ���snd_nxt�����Ըð���snd_una֮ǰ���걸�ģ�snd_nxt֮��δ���ͣ���Ӧ�յ�ack
	if (_itimediff(sn, kcp->snd_una) < 0 || _itimediff(sn, kcp->snd_nxt) >= 0)
		return;

	for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = next) {
		IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
		next = p->next;
		if (sn == seg->sn) {
			iqueue_del(p);
			ikcp_segment_delete(kcp, seg);
			kcp->nsnd_buf--;
			break;
		}
		if (_itimediff(sn, seg->sn) < 0) {
			break;
		}
	}
}

static void ikcp_parse_una(ikcpcb *kcp, IUINT32 una)
{
	struct IQUEUEHEAD *p, *next;
	for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = next) {

		IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
		// ��������չ��Ϊ : IKCPSEG *seg = (IKCPSEG*)(   (char*)((IKCPSEG*)p)  - (size_t) &((IKCPSEG *)0)->node   );

		next = p->next;
		if (_itimediff(una, seg->sn) > 0) {
			iqueue_del(p);
			ikcp_segment_delete(kcp, seg);
			kcp->nsnd_buf--;
		}	else {
			break;
		}
	}
}

// ���ݽ��յ���ack����snd_buf���и��¸���Segment��ack�����Ĵ�����
// ����֮���ж��Ƿ���Ҫ�����ش���
static void ikcp_parse_fastack(ikcpcb *kcp, IUINT32 sn)
{
	struct IQUEUEHEAD *p, *next;

	if (_itimediff(sn, kcp->snd_una) < 0 || _itimediff(sn, kcp->snd_nxt) >= 0)
		return;

	for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = next) {
		IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
		next = p->next;
		if (_itimediff(sn, seg->sn) < 0) {
			break;
		}
		else if (sn != seg->sn) {
			seg->fastack++;
		}
	}
}


//---------------------------------------------------------------------
// ack append
//---------------------------------------------------------------------
static void ikcp_ack_push(ikcpcb *kcp, IUINT32 sn, IUINT32 ts)
{
	size_t newsize = kcp->ackcount + 1;
	IUINT32 *ptr;

	if (newsize > kcp->ackblock) {
		IUINT32 *acklist;
		size_t newblock;

		for (newblock = 8; newblock < newsize; newblock <<= 1);
		acklist = (IUINT32*)ikcp_malloc(newblock * sizeof(IUINT32) * 2);

		if (acklist == NULL) {
			assert(acklist != NULL);
			abort();
		}

		if (kcp->acklist != NULL) {
			size_t x;
			for (x = 0; x < kcp->ackcount; x++) {
				acklist[x * 2 + 0] = kcp->acklist[x * 2 + 0];
				acklist[x * 2 + 1] = kcp->acklist[x * 2 + 1];
			}
			ikcp_free(kcp->acklist);
		}

		kcp->acklist = acklist;
		kcp->ackblock = newblock;
	}

	ptr = &kcp->acklist[kcp->ackcount * 2];
	ptr[0] = sn;
	ptr[1] = ts;
	kcp->ackcount++;
}

static void ikcp_ack_get(const ikcpcb *kcp, int p, IUINT32 *sn, IUINT32 *ts)
{
	if (sn) sn[0] = kcp->acklist[p * 2 + 0];
	if (ts) ts[0] = kcp->acklist[p * 2 + 1];
}


//---------------------------------------------------------------------
// parse data
//---------------------------------------------------------------------
void ikcp_parse_data(ikcpcb *kcp, IKCPSEG *newseg)
{
	struct IQUEUEHEAD *p, *prev;
	IUINT32 sn = newseg->sn;
	int repeat = 0;
	
	if (_itimediff(sn, kcp->rcv_nxt + kcp->rcv_wnd) >= 0 ||
		_itimediff(sn, kcp->rcv_nxt) < 0) {
		ikcp_segment_delete(kcp, newseg);
		return;
	}

	for (p = kcp->rcv_buf.prev; p != &kcp->rcv_buf; p = prev) {
		IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
		prev = p->prev;

		// ����Ƿ�Ϊ�ظ����ݰ�
		if (seg->sn == sn) {
			repeat = 1;
			break;
		}
		if (_itimediff(sn, seg->sn) > 0) {
			break;
		}
	}

	if (repeat == 0) {
		iqueue_init(&newseg->node);
		iqueue_add(&newseg->node, p);
		kcp->nrcv_buf++;
	}	else {
		// ����Ѿ����չ��ˣ�����
		ikcp_segment_delete(kcp, newseg);
	}

#if 0
	ikcp_qprint("rcvbuf", &kcp->rcv_buf);
	printf("rcv_nxt=%lu\n", kcp->rcv_nxt);
#endif

	// move available data from rcv_buf to rcv_queue
	// ɨ��rcv_buf��segment��id����rcv_nxt����rcv_nxt���ƣ�ͬʱsegment�Ƴ�rcv_buff����rcv_queue��rcv_nxt�������Ա�֤rcv_queue���걸��
	while (! iqueue_is_empty(&kcp->rcv_buf)) {
		IKCPSEG *seg = iqueue_entry(kcp->rcv_buf.next, IKCPSEG, node);
		if (seg->sn == kcp->rcv_nxt && kcp->nrcv_que < kcp->rcv_wnd) {
			iqueue_del(&seg->node);
			kcp->nrcv_buf--;
			iqueue_add_tail(&seg->node, &kcp->rcv_queue);
			kcp->nrcv_que++;
			kcp->rcv_nxt++;
		}	else {
			break;
		}
	}

#if 0
	ikcp_qprint("queue", &kcp->rcv_queue);
	printf("rcv_nxt=%lu\n", kcp->rcv_nxt);
#endif

#if 1
//	printf("snd(buf=%d, queue=%d)\n", kcp->nsnd_buf, kcp->nsnd_que);
//	printf("rcv(buf=%d, queue=%d)\n", kcp->nrcv_buf, kcp->nrcv_que);
#endif
}


//---------------------------------------------------------------------
// input data
//
// ikcp_input֮�հ�
//
// KCP�У����հ�������ĳ�Ա���������¼�����
//
// - UInt32 rcv_nxt ��һ��Ҫ���յ����ݰ��ı�š�Ҳ����˵�����֮ǰ�İ����Ѿ���˳��ȫ���յ��ˣ����������յ������ŵİ����ѱ�֤���ݰ��������ԡ�˳���ԣ�
// - UInt32 rcv_wnd ���մ��ڵĴ�С
// - Segment[] rcv_queue ������յ������������ݰ�
// - Segment[] rcv_buf ���յ������ݻ��ȴ�ŵ�rcv_buf�С� ��Ϊ���ݿ��������򵽴ﱾ�صģ����Խ��ܵ������ݻᰴsn˳�����η��뵽��Ӧ��λ���С� 
//	 ��sn�ӵ͵������������ݰ����յ��ˣ����������������ݰ�ת�Ƶ�rcv_queue�С������ͱ�֤�����ݰ���˳���ԡ�
// - UInt32[] acklist �յ�����Ҫ���͵Ļش�ȷ�ϡ� ���յ���ʱ�Ƚ�Ҫ�ش�ack��sn����˶����У���flush�������ٷ���ȥ�� 
//	 acklist�У�һ��ack��(sn, timestampe)Ϊһ��ķ�ʽ�洢����[{sn1, ts1}, { sn2,ts2 } ��] ��[sn1, ts1, sn2, ts2 ��]
//
//
//	ikcp_input��������û�������������ݣ�kcp��������������ݵĽ��գ�
//	��Ҫ�û��Լ�������ص�������������������ݰ��Ľ��գ������յ�����ͨ��input����kcp�С�
//	�����û���������ݣ�kcp���ȶ�����ͷ�����н�����ж����ݰ��Ĵ�С���Ự��ŵ���Ϣ��
//	ͬʱ����Զ�˴��ڴ�С��ͨ������parse_una��ȷ��Զ���յ������ݰ���
//	�����յ������ݰ���snd_buf���Ƴ���Ȼ�����shrink_buf������kcp��snd_una��Ϣ��
//	���ڸ���Զ���Լ��Ѿ�ȷ�ϱ����յ����ݰ���Ϣ��
//	֮����ݲ�ͬ�����ݰ�cmd���ͷֱ����Ӧ�����ݰ� : 
//
//	- IKCP_CMD_ACK  : ��Ӧack����kcpͨ���жϵ�ǰ���յ�ack��ʱ�����ack���ڴ洢�ķ���ʱ���������rtt��rto��ʱ�䡣
//	- IKCP_CMD_PUSH : ��Ӧ���ݰ���kcp���Ȼ��ж�sn���Ƿ񳬳��˵�ǰ�������ܽ��յķ�Χ�����������Χ��ֱ�Ӷ���������ݰ���
//	  ������Ѿ�ȷ�Ͻ��չ����ظ���Ҳֱ�Ӷ�����Ȼ������ת�Ƶ��µ�Segment�У�ͨ��parse_data��Segment����rcv_buf�У�
//	  ��parse_data�����Ȼ���rcv_buf�б���һ�Σ��ж��Ƿ��Ѿ����չ�������ݰ���������ݰ�����������ӵ�rcv_buf�У�֮�󽫿��õ�Segment��ת�Ƶ�rcv_queue�С�
//	- IKCP_CMD_WASK : ��ӦԶ�˵Ĵ���̽���������probe��־����֮���ͱ��ش��ڴ�С��
//	- IKCP_CMD_WINS : ��ӦԶ�˵Ĵ��ڸ��°�������������Ĳ�����
//
//	Ȼ����ݽ��յ���ack����snd_buf���и��¸���Segment��ack�����Ĵ���������֮���ж��Ƿ���Ҫ�����ش���
//	�����д����������Ļָ���
//---------------------------------------------------------------------
int ikcp_input(ikcpcb *kcp, const char *data, long size)
{
	IUINT32 una = kcp->snd_una;
	IUINT32 maxack = 0;
	int flag = 0;

	if (ikcp_canlog(kcp, IKCP_LOG_INPUT)) {
		ikcp_log(kcp, IKCP_LOG_INPUT, "[RI] %d bytes", size);
	}

	if (data == NULL || (int)size < (int)IKCP_OVERHEAD) return -1;

	// Part 1 �𲽽���data�е�����
	while (1) {
		IUINT32 ts, sn, len, una, conv;
		IUINT16 wnd;
		IUINT8 cmd, frg;
		IKCPSEG *seg;

		//** Part 1.1
		//** �����������е�KCPͷ��
		//
		//		KCP Header Format :
		//
		//		4               1   1     2 (Byte)
		//		+---+---+---+---+---+---+---+---+
		//		|     conv      |cmd|frg|  wnd  |
		//		+---+---+---+---+---+---+---+---+
		//		|     ts        |     sn        |
		//		+---+---+---+---+---+---+---+---+
		//		|     una       |     len       |
		//		+---+---+---+---+---+---+---+---+
		//		|                               |
		//		+             DATA              +
		//		|                               |
		//		+---+---+---+---+---+---+---+---+
		//
		if (size < (int)IKCP_OVERHEAD) break;

		data = ikcp_decode32u(data, &conv);
		if (conv != kcp->conv) return -1;

		data = ikcp_decode8u(data, &cmd);
		data = ikcp_decode8u(data, &frg);
		data = ikcp_decode16u(data, &wnd);
		data = ikcp_decode32u(data, &ts);
		data = ikcp_decode32u(data, &sn);
		data = ikcp_decode32u(data, &una);
		data = ikcp_decode32u(data, &len);

		// kcp��ͷһ��24���ֽ�, size��ȥIKCP_OVERHEAD��24���ֽ�Ӧ��Ӧ�ò�С��len
		size -= IKCP_OVERHEAD;
		if ((long)size < (long)len) return -2;

		if (cmd != IKCP_CMD_PUSH && cmd != IKCP_CMD_ACK &&
			cmd != IKCP_CMD_WASK && cmd != IKCP_CMD_WINS) 
			return -3;

		//** Part 1.2
		//** ���Զ�˵Ĵ��ڴ�С
		kcp->rmt_wnd = wnd;

		//** Part 1.3 
		//** ����una������ЩsegmentԶ���յ��ˣ�ɾ��send_buf��С��una��segment
		ikcp_parse_una(kcp, una);

		//** ���±���snd_una���ݣ���snd_buffΪ�գ�snd_unaָ��snd_nxt������ָ��send_buff�׶�
		ikcp_shrink_buf(kcp);

		//** Part 1.4 
		//** ����յ�����Զ�˷�����ACK��
		if (cmd == IKCP_CMD_ACK) {
			if (_itimediff(kcp->current, ts) >= 0) {
				//** ����ack
				//** �˴�ʵ�������ڸ���rto, 
				//** ��Ϊ��ʱ�յ�Զ�˵�ack����������֪��Զ�˵İ���������ʱ�䣬��˿�ͳ�Ƶ�ǰ��������Σ����е���
				ikcp_update_ack(kcp, _itimediff(kcp->current, ts));
			}

			//** �����������ĸ�segment���յ��ˣ������snd_buf���Ƴ�
			ikcp_parse_ack(kcp, sn);

			//** ��Ϊsnd_buf���ܸı��ˣ����µ�ǰ��snd_una
			ikcp_shrink_buf(kcp);

			// ��¼����ack sndֵ
			if (flag == 0) {
				flag = 1;
				maxack = sn;
			}	else {
				if (_itimediff(sn, maxack) > 0) {
					maxack = sn;
				}
			}

			// ��¼sn, rtt, rto
			if (ikcp_canlog(kcp, IKCP_LOG_IN_ACK)) {
				ikcp_log(kcp, IKCP_LOG_IN_DATA, 
					"input ack: sn=%lu rtt=%ld rto=%ld", sn, 
					(long)_itimediff(kcp->current, ts),
					(long)kcp->rx_rto);
			}
		}
		//** Part 1.5
		//** ����յ�����Զ�˷��������ݰ�
		else if (cmd == IKCP_CMD_PUSH) {
			if (ikcp_canlog(kcp, IKCP_LOG_IN_DATA)) {
				ikcp_log(kcp, IKCP_LOG_IN_DATA, 
					"input psh: sn=%lu ts=%lu", sn, ts);
			}

			//** ��������㹻��Ľ��մ���
			if (_itimediff(sn, kcp->rcv_nxt + kcp->rcv_wnd) < 0) {
				//** push��ǰ����ack��Զ�ˣ�����flush�з���ack��ȥ)
				ikcp_ack_push(kcp, sn, ts);

				if (_itimediff(sn, kcp->rcv_nxt) >= 0) {
					seg = ikcp_segment_new(kcp, len);
					seg->conv = conv;
					seg->cmd = cmd;
					seg->frg = frg;
					seg->wnd = wnd;
					seg->ts = ts;
					seg->sn = sn;
					seg->una = una;
					seg->len = len;

					if (len > 0) {
						memcpy(seg->data, data, len);
					}

					ikcp_parse_data(kcp, seg);
				}
			}
		}
		//** Part 1.6
		//** ����յ��İ���Զ�˷�����ѯ�ʴ��ڴ�С�İ�
		else if (cmd == IKCP_CMD_WASK) {
			// ready to send back IKCP_CMD_WINS in ikcp_flush
			// tell remote my window size
			kcp->probe |= IKCP_ASK_TELL;
			if (ikcp_canlog(kcp, IKCP_LOG_IN_PROBE)) {
				ikcp_log(kcp, IKCP_LOG_IN_PROBE, "input probe");
			}
		}
		// wins��֪���ڴ�С
		else if (cmd == IKCP_CMD_WINS) {
			// do nothing
			if (ikcp_canlog(kcp, IKCP_LOG_IN_WINS)) {
				ikcp_log(kcp, IKCP_LOG_IN_WINS,
					"input wins: %lu", (IUINT32)(wnd));
			}
		}
		else {
			return -3;
		}

		data += len;
		size -= len;
	}

	if (flag != 0) {
		// ���ݼ�¼�����ack��sndֵ��ɨ��snd_buff��С��max ack��segment��fastack ++������ָ����ֵ�����������ش�
		ikcp_parse_fastack(kcp, maxack);
	}

	// snd_una��Զ��una�Ƚϣ�snd_una>una����δȷ�ϵ�segment�����ܴ����������, ͨ��cwndӵ�����ڿ���
	// ���1 : cwnd < ssthresh, �������׶Σ�cwnd++���ɷ������������+mss
	// ���2 : ӵ�����ƽ׶�
	if (_itimediff(kcp->snd_una, una) > 0) {
		if (kcp->cwnd < kcp->rmt_wnd) {
			IUINT32 mss = kcp->mss;
			if (kcp->cwnd < kcp->ssthresh) {
				kcp->cwnd++;
				kcp->incr += mss;
			}	else {
				if (kcp->incr < mss) kcp->incr = mss;
				kcp->incr += (mss * mss) / kcp->incr + (mss / 16);
				if ((kcp->cwnd + 1) * mss <= kcp->incr) {
					kcp->cwnd++;
				}
			}
			if (kcp->cwnd > kcp->rmt_wnd) {
				kcp->cwnd = kcp->rmt_wnd;
				kcp->incr = kcp->rmt_wnd * mss;
			}
		}
	}

	return 0;
}


//---------------------------------------------------------------------
// ikcp_encode_seg
//---------------------------------------------------------------------
static char *ikcp_encode_seg(char *ptr, const IKCPSEG *seg)
{
	ptr = ikcp_encode32u(ptr, seg->conv);
	ptr = ikcp_encode8u(ptr, (IUINT8)seg->cmd);
	ptr = ikcp_encode8u(ptr, (IUINT8)seg->frg);
	ptr = ikcp_encode16u(ptr, (IUINT16)seg->wnd);
	ptr = ikcp_encode32u(ptr, seg->ts);
	ptr = ikcp_encode32u(ptr, seg->sn);
	ptr = ikcp_encode32u(ptr, seg->una);
	ptr = ikcp_encode32u(ptr, seg->len);
	return ptr;
}

static int ikcp_wnd_unused(const ikcpcb *kcp)
{
	if (kcp->nrcv_que < kcp->rcv_wnd) {
		return kcp->rcv_wnd - kcp->nrcv_que;
	}
	return 0;
}


//---------------------------------------------------------------------
//	ikcp_flush
//	KCP.flush֮����
//
//	KCP�У��뷢��������ĳ�Ա���������¼�����
//	- UInt32 snd_una ��ǰ δ�յ�ȷ�ϻش��� ���ͳ�ȥ�İ��� ��С��š�Ҳ���Ǵ˱��ǰ�İ����Ѿ��յ�ȷ�ϻش��ˡ�
//	- UInt32 snd_nxt ��һ��Ҫ���ͳ�ȥ�İ����
//	- UInt32 snd_wnd ���ʹ��ڴ�С
//	- UInt32 rmt_wnd Զ�˵Ľ��մ��ڴ�С
//	- Segment[] snd_queue ���Ͷ��С�Ӧ�ò�����ݣ��ڵ���KCP.Send�󣩻����˶����У�KCP��flush��ʱ����ݷ��ʹ��ڵĴ�С���پ��������ٸ�Segment���뵽snd_buf�н��з���
//	- Segment[] snd_buf ���ͻ���ء����ͳ�ȥ�����ݽ��������������У��ȴ�Զ�˵Ļش�ȷ�ϣ����յ�Զ��ȷ�ϴ˰��յ����ٴ�snd_buf�Ƴ�ȥ�� KCP��ÿ��flush��ʱ�򶼻������������е�ÿ��Segment�������ʱ�����ж������ͻ��ط���
//
//	Tips :
//	- snd��send����sender����д�����Ͷ�
//	- rmt��remote����д��Զ��
//	- nxt��next����С
//
//	kcp��flush��ʱ��snd_queue�е������ƶ���snd_buf�У�Ȼ������������ݷ��ͳ�ȥ��
//	
//	����kcp�ᷢ������ack��Ϣ��ÿ��ack��Ϣռ��һ��kcp���ݰ�ͷ�Ĵ�С�����ڴ洢ack��sn��ts��
//	�����ts���ڽ��յ����ݰ�ʱ�洢��acklist�е�ts��Ҳ����Զ�˷������ݰ���ts��
//	
//	������ack��Ϣ��kcp�����鵱ǰ�Ƿ���Ҫ��Զ�˴��ڽ���̽�⡣
//	��Ϊkcp����������������Զ��֪ͨ��ɽ��ܴ��ڵĴ�С��һ��Զ�˽��ܴ���Ϊ0��
//	���ؽ���������Զ�˷������ݣ����޷���Զ�˽���ack�����Ӷ�û�л������Զ�˴��ڴ�С��
//	����������£�kcp��Ҫ���ʹ���̽�����Զ�ˣ���Զ�˻ظ����ڴ�С�󣬺�������ſ��Լ�����
//	Ȼ��kcp�ֱ����֮ǰ���жϣ�ѡ���Ƿ��ʹ���̽����ʹ��ڸ��°���
//	
//	����kcp�����õ�ǰ�ķ��ʹ��ڴ�С�����δ�������������أ�
//	���ڴ�С���з��ͻ����С��Զ�˴��ڴ�С�Լ���������������Ĵ��ڴ�С������
//	����������������أ��������������ڴ�С�����ơ�
//	���շ��ʹ����������ɵĴ�С��kcp����Ҫ���͵�Segment��snd_queue�ƶ���snd_buf�С�
//	
//	Ȼ����¿����ش��Ĵ������ش�ʱ���ӳ٣�Ϊ֮������ݷ�����׼����
//	
//	kcp������snd_buf���У������ͻ���������Ҫ���͵�Segment��һ���ͳ�ȥ����3��������ݰ���Ҫ���ͣ�
//	
//	��һ�η��ͣ������ش�ʱ����Ϣ��
//	�����ش�ʱ�䣺�����ش�ʱ����Ϣ������kcp������ѡ��rto * 2��rto*1.5������¼lost��־��
//	ack����ָ�������������ش����������������������ش�ʱ�䣬��¼change��־��
//	Ȼ����3������µ����ݰ����з��͡���һ�����ݰ��ش�����dead_link����ʱ��
//	kcp��state������Ϊ - 1�������state�����������в�û��ʹ�õ���
//	�²�����ǵ�kcp���ͻ������ڵ����ݶ�ʧ�޷��õ�ackʱ��kcp�������ش���
//	����ش������Ȼʧ�ܣ���ʱ����ͨ���ж�state������snd_buf����Щ���������ݰ���
//	������Զռ����snd_buf�����������ķ��ʹ��ڴ�С��
//	
//	���kcp�������������Ĵ��ڴ�С��
//---------------------------------------------------------------------
void ikcp_flush(ikcpcb *kcp)
{
	IUINT32 current = kcp->current;
	char *buffer = kcp->buffer;
	char *ptr = buffer;
	int count, size, i;
	IUINT32 resent, cwnd;
	IUINT32 rtomin;
	struct IQUEUEHEAD *p;
	int change = 0;
	int lost = 0;
	IKCPSEG seg;

	// 'ikcp_update' haven't been called. 
	if (kcp->updated == 0) return;

	seg.conv = kcp->conv;
	seg.cmd = IKCP_CMD_ACK;
	seg.frg = 0;
	seg.wnd = ikcp_wnd_unused(kcp);
	seg.una = kcp->rcv_nxt;
	seg.len = 0;
	seg.sn = 0;
	seg.ts = 0;

	// flush acknowledges
	count = kcp->ackcount;
	for (i = 0; i < count; i++) {
		size = (int)(ptr - buffer);
		if (size + (int)IKCP_OVERHEAD > (int)kcp->mtu) {
			ikcp_output(kcp, buffer, size);
			ptr = buffer;
		}
		ikcp_ack_get(kcp, i, &seg.sn, &seg.ts);
		ptr = ikcp_encode_seg(ptr, &seg);
	}

	kcp->ackcount = 0;

	// probe window size (if remote window size equals zero)
	if (kcp->rmt_wnd == 0) {
		if (kcp->probe_wait == 0) {
			kcp->probe_wait = IKCP_PROBE_INIT;
			kcp->ts_probe = kcp->current + kcp->probe_wait;
		}	
		else {
			if (_itimediff(kcp->current, kcp->ts_probe) >= 0) {
				if (kcp->probe_wait < IKCP_PROBE_INIT) 
					kcp->probe_wait = IKCP_PROBE_INIT;
				kcp->probe_wait += kcp->probe_wait / 2;
				if (kcp->probe_wait > IKCP_PROBE_LIMIT)
					kcp->probe_wait = IKCP_PROBE_LIMIT;
				kcp->ts_probe = kcp->current + kcp->probe_wait;
				kcp->probe |= IKCP_ASK_SEND;
			}
		}
	}	else {
		kcp->ts_probe = 0;
		kcp->probe_wait = 0;
	}

	// flush window probing commands
	if (kcp->probe & IKCP_ASK_SEND) {
		seg.cmd = IKCP_CMD_WASK;
		size = (int)(ptr - buffer);
		if (size + (int)IKCP_OVERHEAD > (int)kcp->mtu) {
			ikcp_output(kcp, buffer, size);
			ptr = buffer;
		}
		ptr = ikcp_encode_seg(ptr, &seg);
	}

	// flush window probing commands
	if (kcp->probe & IKCP_ASK_TELL) {
		seg.cmd = IKCP_CMD_WINS;
		size = (int)(ptr - buffer);
		if (size + (int)IKCP_OVERHEAD > (int)kcp->mtu) {
			ikcp_output(kcp, buffer, size);
			ptr = buffer;
		}
		ptr = ikcp_encode_seg(ptr, &seg);
	}

	kcp->probe = 0;

	// calculate window size
	cwnd = _imin_(kcp->snd_wnd, kcp->rmt_wnd);
	if (kcp->nocwnd == 0) cwnd = _imin_(kcp->cwnd, cwnd);

	// move data from snd_queue to snd_buf
	while (_itimediff(kcp->snd_nxt, kcp->snd_una + cwnd) < 0) {
		IKCPSEG *newseg;
		if (iqueue_is_empty(&kcp->snd_queue)) break;

		newseg = iqueue_entry(kcp->snd_queue.next, IKCPSEG, node);

		iqueue_del(&newseg->node);
		iqueue_add_tail(&newseg->node, &kcp->snd_buf);
		kcp->nsnd_que--;
		kcp->nsnd_buf++;

		newseg->conv = kcp->conv;
		newseg->cmd = IKCP_CMD_PUSH;
		newseg->wnd = seg.wnd;
		newseg->ts = current;
		newseg->sn = kcp->snd_nxt++;
		newseg->una = kcp->rcv_nxt;
		newseg->resendts = current;
		newseg->rto = kcp->rx_rto;
		newseg->fastack = 0;
		newseg->xmit = 0;
	}

	// calculate resent
	resent = (kcp->fastresend > 0)? (IUINT32)kcp->fastresend : 0xffffffff;
	rtomin = (kcp->nodelay == 0)? (kcp->rx_rto >> 3) : 0;

	// flush data segments
	for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = p->next) {
		IKCPSEG *segment = iqueue_entry(p, IKCPSEG, node);
		int needsend = 0;

		// xmitΪ0����һ�η��ͣ���ֵrto��resendts
		if (segment->xmit == 0) {
			needsend = 1;
			segment->xmit++;
			segment->rto = kcp->rx_rto;
			segment->resendts = current + segment->rto + rtomin;
		}
		// ����segment�ط�ʱ�䣬ȴ����send_buf�У�˵����ʱ��δ�յ�ack����Ϊ��ʧ���ط�
		else if (_itimediff(current, segment->resendts) >= 0) {
			needsend = 1;
			segment->xmit++;
			kcp->xmit++;
			// �����ش�ʱ����Ϣ������kcp������ѡ��rto*2��rto*1.5������¼lost��־��
			if (kcp->nodelay == 0) {
				segment->rto += kcp->rx_rto;
			}	else {
				segment->rto += kcp->rx_rto / 2;
			}
			segment->resendts = current + segment->rto;
			lost = 1;
		}
		// �ﵽ�����ش���ֵ�����·���
		else if (segment->fastack >= resent) {
			needsend = 1;
			segment->xmit++;
			segment->fastack = 0;
			segment->resendts = current + segment->rto;
			change++;
		}

		if (needsend) {
			int size, need;
			segment->ts = current;
			segment->wnd = seg.wnd;
			segment->una = kcp->rcv_nxt;

			size = (int)(ptr - buffer);
			need = IKCP_OVERHEAD + segment->len;

			if (size + need > (int)kcp->mtu) {
				ikcp_output(kcp, buffer, size);
				ptr = buffer;
			}

			ptr = ikcp_encode_seg(ptr, segment);

			if (segment->len > 0) {
				memcpy(ptr, segment->data, segment->len);
				ptr += segment->len;
			}

			if (segment->xmit >= kcp->dead_link) {
				kcp->state = -1;
			}
		}
	}

	// flash remain segments
	size = (int)(ptr - buffer);
	if (size > 0) {
		ikcp_output(kcp, buffer, size);
	}

	// update ssthresh
	// �緢�������ش�������������ֵ����Ϊ��ǰ���ʹ��ڵ�һ�룬
	// ��ӵ�����ڵ���Ϊ ssthresh + resent��resent�Ǵ��������ش��Ķ����Ĵ�����
	// resent��ֵ�������˼�ڱ�Ū���İ������յ���resent�����İ���ack��
	// ����������kcp�ͽ�����ӵ������״̬��
	if (change) {
		IUINT32 inflight = kcp->snd_nxt - kcp->snd_una;
		kcp->ssthresh = inflight / 2;
		if (kcp->ssthresh < IKCP_THRESH_MIN)
			kcp->ssthresh = IKCP_THRESH_MIN;
		kcp->cwnd = kcp->ssthresh + resent;
		kcp->incr = kcp->cwnd * kcp->mss;
	}

	if (lost) {
		kcp->ssthresh = cwnd / 2;
		if (kcp->ssthresh < IKCP_THRESH_MIN)
			kcp->ssthresh = IKCP_THRESH_MIN;
		kcp->cwnd = 1;
		kcp->incr = kcp->mss;
	}

	if (kcp->cwnd < 1) {
		kcp->cwnd = 1;
		kcp->incr = kcp->mss;
	}
}


//---------------------------------------------------------------------
// update state (call it repeatedly, every 10ms-100ms), or you can ask 
// ikcp_check when to call it again (without ikcp_input/_send calling).
// 'current' - current timestamp in millisec. 
//
// kcp��Ҫ�ϲ�ͨ��update������kcp���ݰ��ķ��ͣ�ÿ��������ʱ������interval��������
// interval����ͨ������ikcp_interval�����ã����ʱ����10���뵽5��֮�䣬
// ��ʼĬ��ֵΪ100���롣
// 
// ����ע�⵽һ���ǣ�updated����ֻ���ڵ�һ�ε���ikcp_update����ʱ����Ϊ1��
// Դ����û���ҵ�����Ϊ0�ĵط���Ŀ�����һ����־������
// ���������һ��������֮�����������Ҫѡ���ʱ�䡣
//---------------------------------------------------------------------
void ikcp_update(ikcpcb *kcp, IUINT32 current)
{
	IINT32 slap;

	kcp->current = current;

	if (kcp->updated == 0) {
		kcp->updated = 1;
		kcp->ts_flush = kcp->current;
	}

	slap = _itimediff(kcp->current, kcp->ts_flush);

	if (slap >= 10000 || slap < -10000) {
		kcp->ts_flush = kcp->current;
		slap = 0;
	}

	if (slap >= 0) {
		kcp->ts_flush += kcp->interval;
		if (_itimediff(kcp->current, kcp->ts_flush) >= 0) {
			kcp->ts_flush = kcp->current + kcp->interval;
		}
		ikcp_flush(kcp);
	}
}


//---------------------------------------------------------------------
// Determine when should you invoke ikcp_update:
// returns when you should invoke ikcp_update in millisec, if there 
// is no ikcp_input/_send calling. you can call ikcp_update in that
// time, instead of call update repeatly.
// Important to reduce unnacessary ikcp_update invoking. use it to 
// schedule ikcp_update (eg. implementing an epoll-like mechanism, 
// or optimize ikcp_update when handling massive kcp connections)
//
// check�������ڻ�ȡ�´�update��ʱ�䡣
// �����ʱ�����ϴ�update����µ��´�ʱ���snd_buf�еĳ�ʱ�ش�ʱ�������
// check���̻�Ѱ��snd_buf���Ƿ��г�ʱ�ش������ݣ��������Ҫ�ش���Segment��
// �����ص�ǰʱ�䣬��������һ��update�������ش������ȫ������Ҫ�ش���
// ��������С���ش�ʱ�����ж��´�update��ʱ�䡣
//---------------------------------------------------------------------
IUINT32 ikcp_check(const ikcpcb *kcp, IUINT32 current)
{
	IUINT32 ts_flush = kcp->ts_flush;
	IINT32 tm_flush = 0x7fffffff;
	IINT32 tm_packet = 0x7fffffff;
	IUINT32 minimal = 0;
	struct IQUEUEHEAD *p;

	if (kcp->updated == 0) {
		return current;
	}

	if (_itimediff(current, ts_flush) >= 10000 ||
		_itimediff(current, ts_flush) < -10000) {
		ts_flush = current;
	}

	if (_itimediff(current, ts_flush) >= 0) {
		return current;
	}

	tm_flush = _itimediff(ts_flush, current);

	for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = p->next) {
		const IKCPSEG *seg = iqueue_entry(p, const IKCPSEG, node);
		IINT32 diff = _itimediff(seg->resendts, current);
		if (diff <= 0) {
			return current;
		}
		if (diff < tm_packet) tm_packet = diff;
	}

	minimal = (IUINT32)(tm_packet < tm_flush ? tm_packet : tm_flush);
	if (minimal >= kcp->interval) minimal = kcp->interval;

	return current + minimal;
}



int ikcp_setmtu(ikcpcb *kcp, int mtu)
{
	char *buffer;
	if (mtu < 50 || mtu < (int)IKCP_OVERHEAD) 
		return -1;
	buffer = (char*)ikcp_malloc((mtu + IKCP_OVERHEAD) * 3);
	if (buffer == NULL) 
		return -2;
	kcp->mtu = mtu;
	kcp->mss = kcp->mtu - IKCP_OVERHEAD;
	ikcp_free(kcp->buffer);
	kcp->buffer = buffer;
	return 0;
}

int ikcp_interval(ikcpcb *kcp, int interval)
{
	if (interval > 5000) interval = 5000;
	else if (interval < 10) interval = 10;
	kcp->interval = interval;
	return 0;
}

int ikcp_nodelay(ikcpcb *kcp, int nodelay, int interval, int resend, int nc)
{
	if (nodelay >= 0) {
		kcp->nodelay = nodelay;
		if (nodelay) {
			kcp->rx_minrto = IKCP_RTO_NDL;	
		}	
		else {
			kcp->rx_minrto = IKCP_RTO_MIN;
		}
	}
	if (interval >= 0) {
		if (interval > 5000) interval = 5000;
		else if (interval < 10) interval = 10;
		kcp->interval = interval;
	}
	if (resend >= 0) {
		kcp->fastresend = resend;
	}
	if (nc >= 0) {
		kcp->nocwnd = nc;
	}
	return 0;
}


int ikcp_wndsize(ikcpcb *kcp, int sndwnd, int rcvwnd)
{
	if (kcp) {
		if (sndwnd > 0) {
			kcp->snd_wnd = sndwnd;
		}
		if (rcvwnd > 0) {
			kcp->rcv_wnd = rcvwnd;
		}
	}
	return 0;
}

int ikcp_waitsnd(const ikcpcb *kcp)
{
	return kcp->nsnd_buf + kcp->nsnd_que;
}


// read conv
IUINT32 ikcp_getconv(const void *ptr)
{
	IUINT32 conv;
	ikcp_decode32u((const char*)ptr, &conv);
	return conv;
}


