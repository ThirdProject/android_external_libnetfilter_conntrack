/*
 * (C) 2008 by Pablo Neira Ayuso <pablo@netfilter.org>
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 */

#include "internal/internal.h"
#include "internal/stack.h"
#include <linux/filter.h>

#ifndef SKF_AD_NLATTR
#define SKF_AD_NLATTR		12
#endif

#define NFCT_FILTER_REJECT	0U
#define NFCT_FILTER_ACCEPT	~0U

#if 0
static void show_filter(struct sock_filter *this, int size)
{
	int i;

	for(i=0; i<size; i++)
		printf("(%.4x) code=%.4x jt=%.2x jf=%.2x k=%.8x\n", 
					     i,
					     this[i].code & 0xFFFF,
					     this[i].jt   & 0xFF,
					     this[i].jf   & 0xFF,
					     this[i].k    & 0xFFFFFFFF);
}
#else
static inline void show_filter(struct sock_filter *this, int size) {}
#endif

#define NEW_POS(x) (sizeof(x)/sizeof(struct sock_filter))

static int
nfct_bsf_load_payload_offset(struct sock_filter *this, int pos)
{
	struct sock_filter __code = {
		.code 	= BPF_LD|BPF_IMM,
		.k 	= sizeof(struct nlmsghdr) + sizeof(struct nfgenmsg),
	};
	memcpy(&this[pos], &__code, sizeof(__code));
	return NEW_POS(__code);
}

static int
nfct_bsf_find_attr(struct sock_filter *this, int attr, int pos)
{
	struct sock_filter __code[] = {
		[0] = {
			/* X = attribute type */
			.code	= BPF_LDX|BPF_IMM,
			.k	= attr,
		},
		[1] = {
			/* A = netlink attribute offset */
			.code	= BPF_LD|BPF_B|BPF_ABS,
			.k	= SKF_AD_OFF + SKF_AD_NLATTR,
		}
	};
	memcpy(&this[pos], __code, sizeof(__code));
	return NEW_POS(__code);
}

struct jump {
	int line;
	u_int8_t jt;
	u_int8_t jf;
};

static int
nfct_bsf_cmp_k_stack(struct sock_filter *this, int k, 
	       int jump_true, int pos, struct stack *s)
{
	struct sock_filter __code = {
		.code	= BPF_JMP|BPF_JEQ|BPF_K,
		.k	= k,
	};
	struct jump jmp = {
		.line	= pos,
		.jt	= jump_true - 1,
		.jf	= 0,
	};
	stack_push(s, &jmp);
	memcpy(&this[pos], &__code, sizeof(__code));
	return NEW_POS(__code);
}

static int
nfct_bsf_alu_and(struct sock_filter *this, int k, int pos)
{
	struct sock_filter __code = {
		.code 	= BPF_ALU|BPF_AND|BPF_K,
		.k	= k,
	};
	memcpy(&this[pos], &__code, sizeof(__code));
	return NEW_POS(__code);
}

static int
nfct_bsf_add_attr_data_offset(struct sock_filter *this, int pos)
{
	struct sock_filter __code = {
		/* A += sizeof(struct nfattr) */
		.code	= BPF_ALU|BPF_ADD|BPF_K,
		.k	= sizeof(struct nfattr),
	};
	memcpy(&this[pos], &__code, sizeof(__code));
	return NEW_POS(__code);
}

static int
nfct_bsf_x_equal_a(struct sock_filter *this, int pos)
{
	struct sock_filter __code = {
		.code	= BPF_MISC|BPF_TAX,
	};
	memcpy(&this[pos], &__code, sizeof(__code));
	return NEW_POS(__code);
}

static int
nfct_bsf_load_attr(struct sock_filter *this, int word_size, int pos)
{
	struct sock_filter __code = {
		/* A = skb->data[X + k:word_size] */
		.code	= BPF_LD|word_size|BPF_IND,
		.k	= sizeof(struct nfattr),

	};
	memcpy(&this[pos], &__code, sizeof(__code));
	return NEW_POS(__code);
}

static int
nfct_bsf_ret_verdict(struct sock_filter *this, int verdict, int pos)
{
	struct sock_filter __code = {
		.code	= BPF_RET|BPF_K,
		.k	= verdict,
	};
	memcpy(&this[pos], &__code, sizeof(__code));
	return NEW_POS(__code);
}

static int
nfct_bsf_jump_to(struct sock_filter *this, int line, int pos)
{
	struct sock_filter __code = {
		.code	= BPF_JMP|BPF_JA,
		.k	= line,
	};
	memcpy(&this[pos], &__code, sizeof(__code));
	return NEW_POS(__code);
};

static int
add_state_filter_cta(struct sock_filter *this,
		     unsigned int cta_protoinfo_proto,
		     unsigned int cta_protoinfo_state,
		     u_int16_t state_flags,
		     unsigned int logic)
{
	unsigned int i, j;
	unsigned int label_continue, jt;
	struct stack *s;
	struct jump jmp;

	/* XXX: 32 maximum states + 3 jumps in the three-level iteration */
	s = stack_create(sizeof(struct jump), 3 + 32);
	if (s == NULL) {
		errno = ENOMEM;
		return -1;
	}

	jt = 1;
	if (logic == NFCT_FILTER_LOGIC_POSITIVE)
		label_continue = 1;
	else
		label_continue = 2;

	j = 0;
	j += nfct_bsf_load_payload_offset(this, j);
	j += nfct_bsf_find_attr(this, CTA_PROTOINFO, j);
	j += nfct_bsf_cmp_k_stack(this, 0, label_continue - j, j, s);
	j += nfct_bsf_add_attr_data_offset(this, j);
	j += nfct_bsf_find_attr(this, cta_protoinfo_proto, j);
	j += nfct_bsf_cmp_k_stack(this, 0, label_continue - j, j, s);
	j += nfct_bsf_add_attr_data_offset(this, j);
	j += nfct_bsf_find_attr(this, cta_protoinfo_state, j);
	j += nfct_bsf_cmp_k_stack(this, 0, label_continue - j, j, s);
	j += nfct_bsf_x_equal_a(this, j);
	j += nfct_bsf_load_attr(this, BPF_B, j);

	for (i = 0; i < sizeof(state_flags) * 8; i++) {
		if (state_flags & (1 << i)) {
			j += nfct_bsf_cmp_k_stack(this, i, jt - j, j, s);
		}
	}

	while (stack_pop(s, &jmp) != -1)
		this[jmp.line].jt += jmp.jt + j;

	if (logic == NFCT_FILTER_LOGIC_NEGATIVE)
		j += nfct_bsf_jump_to(this, 1, j);

	j += nfct_bsf_ret_verdict(this, NFCT_FILTER_REJECT, j);

	stack_destroy(s);

	return j;
}

static int 
add_state_filter(struct sock_filter *this, 
		 int proto,
		 u_int16_t flags,
		 unsigned int logic)
{
	struct {
		unsigned int cta_protoinfo;
		unsigned int cta_state;
	} cta[IPPROTO_MAX] = {
		[IPPROTO_TCP] = {
			.cta_protoinfo = CTA_PROTOINFO_TCP,
			.cta_state = CTA_PROTOINFO_TCP_STATE,
		},
		[IPPROTO_SCTP] = {
			.cta_protoinfo = CTA_PROTOINFO_SCTP,
			.cta_state = CTA_PROTOINFO_SCTP_STATE,
		},
		[IPPROTO_DCCP] = {
			.cta_protoinfo = CTA_PROTOINFO_DCCP,
			.cta_state = CTA_PROTOINFO_DCCP_STATE,
		},
	};

	if (cta[proto].cta_protoinfo == 0 && cta[proto].cta_state == 0) {
		errno = ENOTSUP;
		return -1;
	}

	return add_state_filter_cta(this,
				    cta[proto].cta_protoinfo,
				    cta[proto].cta_state,
				    flags,
				    logic);
}

static int 
bsf_add_state_filter(const struct nfct_filter *filter, struct sock_filter *this)
{
	unsigned int i, j;

	for (i = 0, j = 0; i < IPPROTO_MAX; i++) {
		if (filter->l4proto_state[i].map &&
		    filter->l4proto_state[i].len > 0) {
			j += add_state_filter(
				      this, 
				      i, 
				      filter->l4proto_state[i].map,
				      filter->logic[NFCT_FILTER_L4PROTO_STATE]);
		}
	}

	return j;
}

static int 
bsf_add_proto_filter(const struct nfct_filter *f, struct sock_filter *this)
{
	unsigned int i, j;
	unsigned int label_continue, jt;
	struct stack *s;
	struct jump jmp;

	/* nothing to filter, skip */
	if (f->l4proto_len == 0)
		return 0;

	/* XXX: 255 maximum proto + 3 jumps in the three-level iteration */
	s = stack_create(sizeof(struct jump), 3 + 255);
	if (s == NULL) {
		errno = ENOMEM;
		return -1;
	}

	jt = 1;
	if (f->logic[NFCT_FILTER_L4PROTO] == NFCT_FILTER_LOGIC_POSITIVE)
		label_continue = 1;
	else
		label_continue = 2;

	j = 0;
	j += nfct_bsf_load_payload_offset(this, j);
	j += nfct_bsf_find_attr(this, CTA_TUPLE_ORIG, j);
	j += nfct_bsf_cmp_k_stack(this, 0, label_continue - j, j, s);
	j += nfct_bsf_add_attr_data_offset(this, j);
	j += nfct_bsf_find_attr(this, CTA_TUPLE_PROTO, j);
	j += nfct_bsf_cmp_k_stack(this, 0, label_continue - j, j, s);
	j += nfct_bsf_add_attr_data_offset(this, j);
	j += nfct_bsf_find_attr(this, CTA_PROTO_NUM, j);
	j += nfct_bsf_cmp_k_stack(this, 0, label_continue - j, j, s);
	j += nfct_bsf_x_equal_a(this, j);
	j += nfct_bsf_load_attr(this, BPF_B, j);

	for (i = 0; i < IPPROTO_MAX; i++) {
		if (test_bit(i, f->l4proto_map)) {
			j += nfct_bsf_cmp_k_stack(this, i, jt - j, j, s);
		}
	}

	while (stack_pop(s, &jmp) != -1)
		this[jmp.line].jt += jmp.jt + j;

	if (f->logic[NFCT_FILTER_L4PROTO] == NFCT_FILTER_LOGIC_NEGATIVE)
		j += nfct_bsf_jump_to(this, 1, j);

	j += nfct_bsf_ret_verdict(this, NFCT_FILTER_REJECT, j);

	stack_destroy(s);

	return j;
}

static int
bsf_add_addr_ipv4_filter(const struct nfct_filter *f,
		         struct sock_filter *this,
			 unsigned int type)
{
	unsigned int i, j, dir, attr;
	unsigned int label_continue, jt;
	struct stack *s;
	struct jump jmp;

	switch(type) {
	case CTA_IP_V4_SRC:
		dir = __FILTER_ADDR_SRC;
		attr = NFCT_FILTER_SRC_IPV4;
		break;
	case CTA_IP_V4_DST:
		dir = __FILTER_ADDR_DST;
		attr = NFCT_FILTER_DST_IPV4;
		break;
	default:
		return 0;
	}

	/* nothing to filter, skip */
	if (f->l3proto_elems[dir] == 0)
		return 0;

	/* XXX: 127 maximum IPs + 3 jumps in the three-level iteration */
	s = stack_create(sizeof(struct jump), 3 + 127);
	if (s == NULL) {
		errno = ENOMEM;
		return -1;
	}

	jt = 1;
	if (f->logic[attr] == NFCT_FILTER_LOGIC_POSITIVE)
		label_continue = 1;
	else
		label_continue = 2;

	j = 0;
	j += nfct_bsf_load_payload_offset(this, j);
	j += nfct_bsf_find_attr(this, CTA_TUPLE_ORIG, j);
	j += nfct_bsf_cmp_k_stack(this, 0, label_continue - j, j, s);
	j += nfct_bsf_add_attr_data_offset(this, j);
	j += nfct_bsf_find_attr(this, CTA_TUPLE_IP, j);
	j += nfct_bsf_cmp_k_stack(this, 0, label_continue - j, j, s);
	j += nfct_bsf_add_attr_data_offset(this, j);
	j += nfct_bsf_find_attr(this, type, j);
	j += nfct_bsf_cmp_k_stack(this, 0, label_continue - j, j, s);
	j += nfct_bsf_x_equal_a(this, j);
	j += nfct_bsf_load_attr(this, BPF_W, j);

	for (i = 0; i < f->l3proto_elems[dir]; i++) {
		int ip = f->l3proto[dir][i].addr & f->l3proto[dir][i].mask;

		j += nfct_bsf_alu_and(this, f->l3proto[dir][i].mask, j);
		j += nfct_bsf_cmp_k_stack(this, ip, jt - j, j, s);
	}

	while (stack_pop(s, &jmp) != -1)
		this[jmp.line].jt += jmp.jt + j;

	if (f->logic[attr] == NFCT_FILTER_LOGIC_NEGATIVE)
		j += nfct_bsf_jump_to(this, 1, j);

	j += nfct_bsf_ret_verdict(this, NFCT_FILTER_REJECT, j);

	stack_destroy(s);

	return j;
}

static int
bsf_add_saddr_ipv4_filter(const struct nfct_filter *f, struct sock_filter *this)
{
	return bsf_add_addr_ipv4_filter(f, this, CTA_IP_V4_SRC);
}

static int 
bsf_add_daddr_ipv4_filter(const struct nfct_filter *f, struct sock_filter *this)
{
	return bsf_add_addr_ipv4_filter(f, this, CTA_IP_V4_DST);
}

/* this buffer must be big enough to store all the autogenerated lines */
#define BSF_BUFFER_SIZE 	2048

int __setup_netlink_socket_filter(int fd, struct nfct_filter *f)
{
	struct sock_filter bsf[BSF_BUFFER_SIZE];	
	struct sock_fprog sf;	
	unsigned int j = 0;

	memset(bsf, 0, sizeof(bsf));

	j += bsf_add_proto_filter(f, &bsf[j]);
	j += bsf_add_saddr_ipv4_filter(f, &bsf[j]);
	j += bsf_add_daddr_ipv4_filter(f, &bsf[j]);
	j += bsf_add_state_filter(f, &bsf[j]);

	/* nothing to filter, skip */
	if (j == 0)
		return 0;

	j += nfct_bsf_ret_verdict(bsf, NFCT_FILTER_ACCEPT, j);

	show_filter(bsf, j);

	sf.len = (sizeof(struct sock_filter) * j) / sizeof(bsf[0]);
	sf.filter = bsf;

	return setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &sf, sizeof(sf));
}
