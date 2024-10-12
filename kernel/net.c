#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static struct spinlock netlock;

struct sock {
  uint16 local_port;
  uint16 remote_port;
  uint32 remote_addr;
  struct proc *proc;
  struct spinlock lock;
  struct recvq queue;
  uint32 size;
  struct sock *next;
};

static struct sock *sockets;

void
netinit(void)
{
  initlock(&netlock, "netlock");
  sockets = 0;
}


//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
uint64
sys_bind(void)
{
  int port;
  argint(0, &port);
  if (port < 0 || port >= (1 << 16))
    return -1;

  struct sock *sock, *ptr;
  if ((sock = (struct sock *) kalloc()) == 0)
    return -1;

  memset(sock, 0, PGSIZE);
  sock->local_port = port;
  sock->size = 0;
  sock->proc = myproc();
  initlock(&sock->lock, "socket");

  acquire(&netlock);
  // check port not being used
  ptr = sockets;
  while (ptr) {
    if (ptr->local_port == port) {
      release(&netlock);
      kfree(sock);
      return -1;
    }
    ptr = ptr->next;
  }

  sock->next = sockets;
  sockets = sock;
  release(&netlock);
  return 0;
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64
sys_unbind(void)
{
  int port;
  argint(0, &port);
  if (port < 0 || port >= (1 << 16))
    return -1;
  if (!sockets)
    return -1;

  struct proc *p = myproc();
  struct sock **pp, *curr;
  struct packet *pkt, *next;

  acquire(&netlock);
  pp = &sockets;
  curr = sockets;
  while (curr) {
    if (curr->local_port == port && curr->proc == p) {
      acquire(&curr->lock);
      pkt = curr->queue.head;
      while (pkt) {
        next = pkt->next;
        kfree(pkt);
        pkt = next;
      }
      release(&curr->lock);

      *pp = curr->next;
      kfree(curr);
      release(&netlock);
      return 0;
    }
    pp = &curr->next;
    curr = curr->next;
  }
  release(&netlock);
  return -1;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
// if there's a received UDP packet already queued that was
// addressed to dport, then return it.
// otherwise wait for such a packet.
//
// sets *src to the IP source address.
// sets *sport to the UDP source port.
// copies up to maxlen bytes of UDP payload to buf.
// returns the number of bytes copied,
// and -1 if there was an error.
//
// dport, *src, and *sport are host byte order.
// bind(dport) must previously have been called.
//
uint64
sys_recv(void)
{
  int dst_port;
  uint64 src_addr;
  uint64 src_port;
  uint64 buf_addr;
  int maxlen;
  struct sock *sock;
  struct proc *p = myproc();

  argint(0, &dst_port);
  argaddr(1, &src_addr);
  argaddr(2, &src_port);
  argaddr(3, &buf_addr);
  argint(4, &maxlen);

  acquire(&netlock);
  sock = sockets;
  while (sock) {
    if (sock->local_port == dst_port) {
      release(&netlock);
      acquire(&sock->lock);

      // if no packets queued, wait until one arrives
      while (sock->size == 0) {
        sleep(&sock->queue, &sock->lock);
      }

      // dequeue the packet
      struct packet *pkt = sock->queue.head;
      sock->queue.head = pkt->next;
      if (sock->queue.head == 0)
        sock->queue.tail = 0;
      sock->size -= 1;

      release(&sock->lock);

      int copylen = pkt->len < maxlen ? pkt->len : maxlen;

      // Copy data to user space
      if (copyout(p->pagetable, buf_addr, pkt->buf, copylen) < 0 ||
          copyout(p->pagetable, src_addr, (char *) &sock->remote_addr, sizeof(uint32)) < 0 ||
          copyout(p->pagetable, src_port, (char *) &sock->remote_port, sizeof(uint16)) < 0) {
        kfree(pkt);
        return -1;
      }

      kfree(pkt);
      return copylen;
    }
    sock = sock->next;
  }
  release(&netlock);
  return -1;
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45; // version 4, header length 4*5
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  e1000_transmit(buf, total);

  return 0;
}

void
enqueue(struct sock *sock, char *buf, int len, uint32 ip, uint16 rport)
{
  // Queue is full; drop the packet
  if (sock->size >= 16) {
    kfree(buf);
    return;
  }

  struct packet *packet = (struct packet *) kalloc();
  if (!packet) {
    kfree(buf);
    return;
  }

  memmove(packet->buf, buf + HDRLEN, len);
  packet->len = len;
  packet->next = 0;
  kfree(buf);

  acquire(&sock->lock);
  if (sock->size == 0) {
    sock->remote_addr = ip;
    sock->remote_port = rport;
    sock->queue.head = packet;
    sock->queue.tail = packet;
  } else {
    sock->queue.tail->next = packet;
    sock->queue.tail = packet;
  }
  sock->size += 1;
  wakeup(&sock->queue);
  release(&sock->lock);
}

void
sockrecvudp(char *buf, int len, uint32 ip, uint16 rport, uint16 lport)
{
  struct sock *ptr;
  acquire(&netlock);
  ptr = sockets;
  while (ptr) {
    if (ptr->local_port == lport) {
      release(&netlock);
      enqueue(ptr, buf, len, ip, rport);
      return;
    }
    ptr = ptr->next;
  }
  release(&netlock);
  kfree(buf);
}

void
ip_rx(char *buf, int len)
{
  // don't delete this printf; make grade depends on it.
  static int seen_ip = 0;
  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  //
  // Your code here.
  //
  if (len < HDRLEN)
    goto fail;

  struct eth *eth = (struct eth *) buf;
  struct ip *ip = (struct ip *) (eth + 1);
  struct udp *udp = (struct udp *) (ip + 1);
  uint32 src_ip;
  uint16 src_port, dst_port;

  // check ip version and header length
  if (ip->ip_vhl != ((4 << 4) | (20 >> 2)))
    goto fail;
  // does not support fragmented packets
  if (ntohs(ip->ip_off) != 0)
    goto fail;
  // only supports UDP packet
  if (ip->ip_p != IPPROTO_UDP)
    goto fail;
  // only reads packet addressed to the local machine
  if (ntohl(ip->ip_dst) != local_ip)
    goto fail;

  len = ntohs(udp->ulen) - sizeof(struct udp);
  src_ip = ntohl(ip->ip_src);
  src_port = ntohs(udp->sport);
  dst_port = ntohs(udp->dport);
  sockrecvudp(buf, len, src_ip, src_port, dst_port);
  return;

  fail:
    kfree(buf);
}

//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *) inbuf;
  struct arp *inarp = (struct arp *) (ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");
  
  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN); // ethernet destination = query source
  memmove(eth->shost, local_mac, ETHADDR_LEN); // ethernet source = xv6's ethernet address
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  e1000_transmit(buf, sizeof(*eth) + sizeof(*arp));

  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *) buf;

  if (len >= sizeof(struct eth) + sizeof(struct arp) && ntohs(eth->type) == ETHTYPE_ARP) {
    arp_rx(buf);
  } else if (len >= sizeof(struct eth) + sizeof(struct ip) && ntohs(eth->type) == ETHTYPE_IP) {
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
