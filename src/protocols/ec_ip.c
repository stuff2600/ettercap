/*
    ettercap -- IP decoder module

    Copyright (C) ALoR & NaGA

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

    $Header: /home/drizzt/dev/sources/ettercap.cvs/ettercap_ng/src/protocols/ec_ip.c,v 1.16 2003/09/17 10:57:40 lordnaga Exp $
*/

#include <ec.h>
#include <ec_inet.h>
#include <ec_decode.h>
#include <ec_fingerprint.h>
#include <ec_checksum.h>
#include <ec_session.h>


/* globals */

struct ip_header {
#ifndef WORDS_BIGENDIAN
   u_int8   ihl:4;
   u_int8   version:4;
#else 
   u_int8   version:4;
   u_int8   ihl:4;
#endif
   u_int8   tos;
   u_int16  tot_len;
   u_int16  id;
   u_int16  frag_off;
#define IP_DF 0x4000
#define IP_MF 0x2000
#define IP_FRAG 0x1fff
   u_int8   ttl;
   u_int8   protocol;
   u_int16  csum;
   u_int32  saddr;
   u_int32  daddr;
/*The options start here. */
};

/* Session data structure */
struct ip_status {
   u_int16  last_id;
   int16    id_adj;
};

/* Ident structure for ip sessions */
struct ip_ident {
   u_int32 magic;
      #define IP_MAGIC  0x0300e77e
   struct ip_addr L3_src;
};

#define IP_IDENT_LEN sizeof(struct ip_ident)


/* protos */

FUNC_DECODER(decode_ip);
void ip_init(void);
int ip_match(void *id_sess, void *id_curr);
void ip_create_session(struct session **s, struct packet_object *po);
size_t ip_create_ident(void **i, struct packet_object *po);            


/*******************************************/

/*
 * this function is the initializer.
 * it adds the entry in the table of registered decoder
 */

void __init ip_init(void)
{
   add_decoder(NET_LAYER, LL_TYPE_IP, decode_ip);
}


FUNC_DECODER(decode_ip)
{
   FUNC_DECODER_PTR(next_decoder);
   struct ip_header *ip;
   struct session *s = NULL;
   void *ident = NULL;
   struct ip_status *status;

   ip = (struct ip_header *)DECODE_DATA;
  
   DECODED_LEN = ip->ihl * 4;

   /* IP addresses */
   ip_addr_init(&PACKET->L3.src, AF_INET, (char *)&ip->saddr);
   ip_addr_init(&PACKET->L3.dst, AF_INET, (char *)&ip->daddr);
   
   /* this is needed at upper layer to calculate the tcp payload size */
   PACKET->L3.payload_len = ntohs(ip->tot_len) - DECODED_LEN;

   /* other relevant infos */
   PACKET->L3.header = (u_char *)DECODE_DATA;
   PACKET->L3.len = DECODED_LEN;
   
   /* parse the options */
   if (ip->ihl * 4 != sizeof(struct ip_header)) {
      PACKET->L3.options = (u_char *)(DECODE_DATA) + sizeof(struct ip_header);
      PACKET->L3.optlen = (ip->ihl * 4) - sizeof(struct ip_header);
   } else {
      PACKET->L3.options = NULL;
      PACKET->L3.optlen = 0;
   }
   
   PACKET->L3.proto = htons(LL_TYPE_IP);
   PACKET->L3.ttl = ip->ttl;
  
   /* XXX - implement the handling of fragmented packet */
   /* don't process fragmented packets */
   if (ntohs(ip->frag_off) & IP_FRAG || ntohs(ip->frag_off) & IP_MF)
      return NULL;
   
   /* 
    * if the checsum is wrong, don't parse it (avoid ettercap spotting) 
    * the checksum should be 0 ;)
    */
   if (L3_checksum(PACKET) != 0) {
      USER_MSG("Invalid IP packet from %s : csum [%#x] (%#x)\n", int_ntoa(ip->saddr), 
                              L3_checksum(PACKET), ntohs(ip->csum));
      return NULL;
   }
   
   /* if it is a TCP packet, try to passive fingerprint it */
   if (ip->protocol == NL_TYPE_TCP) {
      /* initialize passive fingerprint */
      fingerprint_default(PACKET->PASSIVE.fingerprint);
  
      /* collect infos for passive fingerprint */
      fingerprint_push(PACKET->PASSIVE.fingerprint, FINGER_TTL, ip->ttl);
      fingerprint_push(PACKET->PASSIVE.fingerprint, FINGER_DF, ntohs(ip->frag_off) & IP_DF);
      fingerprint_push(PACKET->PASSIVE.fingerprint, FINGER_LT, ip->ihl * 4);
   }

   /* calculate if the dest is local or not */
   switch (ip_addr_is_local(&PACKET->L3.src)) {
      case ESUCCESS:
         PACKET->PASSIVE.flags |= FP_HOST_LOCAL;
         break;
      case -ENOTFOUND:
         PACKET->PASSIVE.flags |= FP_HOST_NONLOCAL;
         break;
      case -EINVALID:
         PACKET->PASSIVE.flags = FP_UNKNOWN;
         break;
   }
   
   /* HOOK POINT: PACKET_IP */
   hook_point(PACKET_IP, po);

   /* Find or create the correct session */
   ip_create_ident(&ident, PACKET);
   if (session_get(&s, ident, IP_IDENT_LEN) == -ENOTFOUND) {
      ip_create_session(&s, PACKET);
      session_put(s);
   }
   SAFE_FREE(ident);
   
   /* Record last packet's ID */
   status = (struct ip_status *)s->data;
   status->last_id = ip->id;
      
   /* Jump to next Layer */
   next_decoder = get_decoder(PROTO_LAYER, ip->protocol);
   EXECUTE_DECODER(next_decoder);
   
   /* 
    * Modification checks and adjustments.
    * - ip->id according to number of injected/dropped packets
    * - ip->len according to upper layer's payload modification
    */
   if (PACKET->flags & PO_DROPPED)
      status->id_adj--;
   else if ((PACKET->flags & PO_MODIFIED) || (status->id_adj != 0)) {
      
      /* se the correct id for this packet */
      ORDER_ADD_SHORT(ip->id, status->id_adj);
      /* adjust the packet length */
      ORDER_ADD_SHORT(ip->tot_len, PACKET->delta);

      /* 
       * In case some upper level encapsulated 
       * ip decoder modified it... (required for checksum)
       */
      PACKET->L3.header = (u_char *)DECODE_DATA;
      PACKET->L3.len = DECODED_LEN;
   
      /* ...recalculate checksum */
      ip->csum = 0; 
      ip->csum = L3_checksum(PACKET);
   }

   /*
    * External L3 header sets itself 
    * as the packet to be forwarded.
    */
   PACKET->fwd_packet = (u_char *)DECODE_DATA;
   PACKET->fwd_len = ntohs(ip->tot_len);
      
   return NULL;
}


/*******************************************/

/* Sessions' stuff for ip packets */


/*
 * create the ident for a session
 */
 
size_t ip_create_ident(void **i, struct packet_object *po)
{
   struct ip_ident *ident;
   
   /* allocate the ident for that session */
   ident = calloc(1, sizeof(struct ip_ident));
   ON_ERROR(ident, NULL, "can't allocate memory");
  
   /* the magic */
   ident->magic = IP_MAGIC;
      
   /* prepare the ident */
   memcpy(&ident->L3_src, &po->L3.src, sizeof(struct ip_addr));

   /* return the ident */
   *i = ident;

   /* return the lenght of the ident */
   return sizeof(struct ip_ident);
}


/*
 * compare two session ident
 *
 * return 1 if it matches
 */

int ip_match(void *id_sess, void *id_curr)
{
   struct ip_ident *ids = id_sess;
   struct ip_ident *id = id_curr;

   /* sanity check */
   BUG_IF(ids == NULL);
   BUG_IF(id == NULL);
  
   /* 
    * is this ident from our level ?
    * check the magic !
    */
   if (ids->magic != id->magic)
      return 0;
   
   /* Check the source */
   if ( !ip_addr_cmp(&ids->L3_src, &id->L3_src) ) 
      return 1;

   return 0;
}

/*
 * prepare the ident and the pointer to match function
 * for ip layer.
 */

void ip_create_session(struct session **s, struct packet_object *po)
{
   void *ident;

   DEBUG_MSG("ip_create_session");
   
   /* allocate the session */
   *s = calloc(1, sizeof(struct session));
   ON_ERROR(*s, NULL, "can't allocate memory");
   
   /* create the ident */
   (*s)->ident_len = ip_create_ident(&ident, po);
   
   /* link to the session */
   (*s)->ident = ident;

   /* the matching function */
   (*s)->match = &ip_match;
   
   /* alloc of data element */
   (*s)->data = calloc(1, sizeof(struct ip_status));
}

/* EOF */

// vim:ts=3:expandtab

