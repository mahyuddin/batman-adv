#ifndef _NET_BATMAN_ADV_COMPAT_NET_MLD_H_
#define _NET_BATMAN_ADV_COMPAT_NET_MLD_H_

#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 35)
#include_next <net/mld.h>
#endif /* >= KERNEL_VERSION(2, 6, 35) */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)
struct mld_msg {
	struct icmp6hdr		mld_hdr;
	struct in6_addr		mld_mca;
};

#define mld_type		mld_hdr.icmp6_type

struct mld2_grec {
	__u8		grec_type;
	__u8		grec_auxwords;
	__be16		grec_nsrcs;
	struct in6_addr	grec_mca;
	struct in6_addr	grec_src[0];
};

struct mld2_report {
	struct icmp6hdr		mld2r_hdr;
	struct mld2_grec	mld2r_grec[0];
};

struct mld2_query {
	struct icmp6hdr		mld2q_hdr;
	struct in6_addr		mld2q_mca;
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u8			mld2q_qrv:3,
				mld2q_suppress:1,
				mld2q_resv2:4;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u8			mld2q_resv2:4,
				mld2q_suppress:1,
				mld2q_qrv:3;
#else
#error "Please fix <asm/byteorder.h>"
#endif
	__u8			mld2q_qqic;
	__be16			mld2q_nsrcs;
	struct in6_addr		mld2q_srcs[0];
};

#endif /* < KERNEL_VERSION(2, 6, 35) */

#endif	/* _NET_BATMAN_ADV_COMPAT_NET_MLD_H_ */
