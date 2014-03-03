#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"

void send_packet(rel_t *s);
int convert_read_and_check(packet_t* pkt, size_t len);

struct reliable_state {
   rel_t *next;            /* Linked list for traversing all connections */
   rel_t **prev;
   conn_t *c;          /* This is the connection object */
 
   /* Add your own data fields below this */
   
    //sender data fields
   packet_t latest_packet;
   int last_frame_sent; 
   int sender_is_empty;

   //receiver data fields
   int length_received;
   int last_frame_received; 
   char data_received[500];
};

rel_t *rel_list;


/* Creates a new reliable protocol session, returns NULL on failure.
* Exactly one of c and ss should be NULL.  (ss is NULL when called
* from rlib.c, while c is NULL when this function is called from
* rel_demux.) */

rel_t *

rel_create (conn_t *c, const struct sockaddr_storage *ss,
           const struct config_common *cc)
{
   rel_t *r;
   
   r = xmalloc (sizeof (*r));
   memset (r, 0, sizeof (*r));
   
   if (!c) {
       c = conn_create (r, ss);
       if (!c) {
           free (r);
           return NULL;
       }
   }
   
   r->c = c;
   r->next = rel_list;
   r->prev = &rel_list;
   if (rel_list)
       rel_list->prev = &r->next;
   rel_list = r;
   
   /* Do any other initialization you need here */

   //Initialize new packet
   packet_t new_packet;
   new_packet.cksum = 0;
   new_packet.len = 12;
   new_packet.ackno = 1;
   new_packet.seqno = 0;

   r->latest_packet = new_packet;
   r->sender_is_empty = 1;
   r->last_frame_sent = 0;

   //initialize receiver data fields
   r->length_received = 0;
   r->last_frame_received = 0;

   return r;

}


void rel_destroy (rel_t *r)
{
    /* Free any other allocated memory here */
   if (r->next!= NULL)
       r->next->prev = r->prev;
   (*r->prev)->next = r->next;
   conn_destroy (r->c);
   
}


/* This function only gets called when the process is running as a
* server and must handle connections from multiple clients.  You have
* to look up the rel_t structure based on the address in the
* sockaddr_storage passed in.  If this is a new connection (sequence
* number 1), you will need to allocate a new conn_t using rel_create
* ().  (Pass rel_create NULL for the conn_t, so it will know to
* allocate a new connection.)
*/

void rel_demux (const struct config_common *cc,
          const struct sockaddr_storage *ss,
          packet_t *pkt, size_t len)
{
}


void rel_recvpkt (rel_t *r, packet_t *pkt, size_t n) {
    //Convert from network to host; return type of data packet
   int type = convert_read_and_check(pkt, n);

   //If data packet is corrupted, return
   if (type == -1) return; 

    //If packet has valid ackno, read packet
   if (r->last_frame_sent == pkt->ackno-1) { //has valid ackno
       r->sender_is_empty = 1;
       rel_read(r);
   } 

   //If data packet, in sequence and can be processed by receiver, add payload to current buffer and output payload.
   if (type == 2 && r->length_received == 0 && pkt->seqno == r->last_frame_received+1) { 
       memcpy(r->data_received, pkt->data, pkt->len);
       r->length_received = pkt->len - 12;
       rel_output(r);
   }
}

/* Convert from network to host, return data packet type*/
int convert_read_and_check(packet_t* pkt, size_t len) {   
   int old_cksum = pkt->cksum;
   int packet_length = ntohs(pkt->len);
   pkt->cksum = 0;

   //if data is corrupted, return -1
   if(len < packet_length || (cksum(pkt, packet_length) != old_cksum)) { \
       return -1;
   }

   // Convert from network to host
   pkt->len = packet_length;
   pkt->ackno = ntohl(pkt->ackno);

   //if ack packet, return 0
   if(pkt->len == 8) {
       return 0;
   }

   //If empty data packet, return 1
   if (pkt->len == 12) { //means teardown
       return 1;
   }
   
   //If data packet with payload, return 2
   pkt->seqno = ntohl(pkt->seqno);
   return 2;
}

/*To get the data that you must transmit to the receiver, keep calling conn_input until it drains.
 When the window is full, break from the loop even if there could still be available data from conn_input. 
 When later some ack packets are received and some slots in sender's window become vacant again, call rel_read. */
void rel_read (rel_t *s) {
    //if sender is not empty, return
   if (s->sender_is_empty==0) return;

   s->latest_packet.len = 12;
   s->latest_packet.cksum = 0;

   int packet_size = conn_input(s->c, s->latest_packet.data, 499);

   //If EOF, reset packet data
   if (packet_size == -1) {
        s->latest_packet.seqno++;
        s->latest_packet.len = 0;
        s->latest_packet.ackno = 1;
        send_packet(s);

   //If payload in data packet, increase sender packet seq number and size
   } else if (packet_size > 0) {
       s->latest_packet.seqno++;
       s->sender_is_empty = 0;
       s->latest_packet.len += packet_size;
       send_packet(s);
       s->last_frame_sent++;
   }
}


/* To output the data you have received in decoded UDP packets, call conn_output which outputs data to STDOUT. 
Flow control the sender by not acknowledging packets if there is no buffer space available for conn_output. 
Library calls rel_output when output has drained, at which point you call conn_bufspace to see how much buffer space you have and
send out more Acks to get more data from the remote side. */

void rel_output (rel_t *r)
{
    //if there is sufficient buffer space, conn_output
   if (conn_bufspace(r->c) >= r->length_received && r->length_received > 0) {
       conn_output(r->c, r->data_received, r->length_received);
       r->last_frame_received++; //increase tracker of last frame received
       r->length_received = 0; //clear length received
       if (r->sender_is_empty==1){ //if sender is empty, set ack packet to get more data.
            r->latest_packet.len = 8; 
       }
       send_packet(r);
   }

}

/* Called periodically, currently at a rate 1/5 of the retransmission interval. 
Use timer to inspect packets and retransmit packets that have not been acknowledged. 
Do not retransmit every packet every time the timer is fired! 
Must keep track of which packets need to be retransmitted and when. */

void rel_timer ()
{
    /* Retransmit any packets that need to be retransmitted */
   rel_t *current = rel_list;
   //Continuously inspect state
   while (current != NULL) {
       if (current->sender_is_empty==0) {
           send_packet(current);
       }
       current = current->next;
   }
}

/* Send latest packet with increased ackno and byte order conversion */
void send_packet(rel_t *s) {
   s->latest_packet.ackno = s->last_frame_received+1;
   packet_t packet = s->latest_packet;
   int len = packet.len;
   
   packet.len = htons(packet.len);
   packet.ackno = htonl(packet.ackno);
   //convert seqno if data packet
   if(packet.len >= 12) {
       packet.seqno = htonl(packet.seqno);
   }
   packet.cksum = cksum(&packet, len);

   if (conn_sendpkt (s->c, &packet, len) != len) {
     exit(1);
   }
}