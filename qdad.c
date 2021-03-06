#include "contiki.h"
#include "net/rime/rime.h"
#include "random.h"
#include "lib/list.h"
#include "lib/memb.h"
#include <stdio.h>

/*--------------------------- GLOBAL VARIABLES ------------------------------*/
// Default values from assignment
#define NODE_TRAVERSAL_TIME 40
#define ADDRESS_RETRIES 3
#define NET_DIAMETER 10
#define ADDRESS_DISCOVERY 3*NODE_TRAVERSAL_TIME*NET_DIAMETER/2
#define REVERSE_ROUTE_LIFETIME ADDRESS_DISCOVERY*2
#define UID_TIMEOUT 2*ADDRESS_DISCOVERY

// Custom global variables
#define MAX_T_START 10 //seconds
#define MAX_ENTRIES 1000
#define MAX_HOPS 50

// States
#define NO_ADDRESS_STATE 0
#define ADVERTISING_STATE 1
#define NORMAL_STATE 2

// Actual state for FSM (NO_ADDRESS_STATE by default)
int actual_state = NO_ADDRESS_STATE;

/*-------------------------- STRUCTS DEFINITION ------------------------------*/
// AREQ package format
struct areq
{
  linkaddr_t originators_address;
  linkaddr_t requested_address;
  uint8_t hops;
};

// Buffer of AREQ packages received
struct areq_memory
{
  struct areq_memory *next;
  linkaddr_t originators_address;
  linkaddr_t requested_address;
  struct ctimer ctimer;
};

// Reverse route for AREQ packages
struct reverse_route_areq
{
  struct reverse_route_areq *next;
  linkaddr_t originators_address;
  linkaddr_t next_hop;
  struct ctimer ctimer;
};

//Lists for AREQ messages received and reverse route
LIST(list_areq);
LIST(list_areq_reverse_route);

// This macro is used to statically declare a block of memory that can be used
// by the block allocation functions.
MEMB(mem_areq, struct areq_memory, MAX_ENTRIES);
MEMB(mem_areq_reverse, struct reverse_route_areq, MAX_ENTRIES);

/*---------------------------------------------------------------------------*/
PROCESS(qdad_process, "QDAD");
AUTOSTART_PROCESSES(&qdad_process);

static struct multihop_conn multihop;
static struct broadcast_conn broadcast;

/*------------------------- BROADCAST FUNCTIONS  -----------------------------*/

static void areq_remove(void *a) {
  printf("  [AREQ_REMOVE] Removing  AREQ message from memory...\n");
  struct areq_memory *am = a;

  list_remove(list_areq, am);
  memb_free(&mem_areq, am);

  return;
}

static void reverse_route_areq_remove(void *a) {
  printf("  [REVERSE_ROUTE_AREQ_REMOVE] Removing AREQ message from reverse path...\n");
  struct reverse_route_areq *rra = a;

  list_remove(list_areq_reverse_route, rra);
  memb_free(&mem_areq_reverse, rra);

  return;
}

static void broadcast_recv(struct broadcast_conn *c, const linkaddr_t *from)
{
  printf("[BROADCAST_RECV] Entering function...\n");
  struct areq *areq_recv;
  struct areq_memory *areq_mem;
  struct reverse_route_areq *rra;

  // Check if we are in NORMAL_STATE
  if(actual_state != NORMAL_STATE)
  {
    printf("  [BROADCAST_RECV] Actual state is not NORMAL_STATE, discard message\n");
    printf("[BROADCAST_RECV] Leaving function...\n");
    return;
  }

  // AREQ received
  areq_recv = packetbuf_dataptr();
  printf("  [BROADCAST_RECV] Broadcast message received from %d.%d\n",
        from->u8[0], from->u8[1]);
  printf("  [BROADCAST_RECV] Originator's address:  %d.%d\n",
        areq_recv->originators_address.u8[0],
        areq_recv->originators_address.u8[1]);
  printf("  [BROADCAST_RECV] Requested address:  %d.%d\n",
        areq_recv->requested_address.u8[0],
        areq_recv->requested_address.u8[1]);

  // Check if originator address is the same as our address (not rebroadcast)
  if (linkaddr_cmp(&linkaddr_node_addr, &areq_recv->originators_address))
  {
    printf("  [BROADCAST_RECV] Originator address is the same as ours, not rebroadcast the message!\n");
    printf("[BROADCAST_RECV] Leaving function...\n");
    return;
  }

  // Check if requested_address is equals to our address
  if (linkaddr_cmp(&linkaddr_node_addr, &areq_recv->requested_address))
  {
    //Saving reverse route path
    printf("  [BROADCAST_RECV] Address received is equal to ours, notify to this bastard!!!\n");
    printf("  [BROADCAST_RECV] Add reverse route to memory\n");
    rra = memb_alloc(&mem_areq_reverse);
    if (rra != NULL)
    {
      linkaddr_copy(&rra->originators_address, &areq_recv->originators_address);
      linkaddr_copy(&rra->next_hop, from);
      list_add(list_areq_reverse_route, rra);
      ctimer_set(&rra->ctimer, REVERSE_ROUTE_LIFETIME, reverse_route_areq_remove, rra);
    }else
    {
      printf("  [BROADCAST_RECV] ALLOCATE MEMORY ERROR\n");
    }

    // Send multicast message to origin
    packetbuf_clear();
    packetbuf_copyfrom("Address is not unique!", 23);

    multihop_send(&multihop, &rra->originators_address);

    printf("[BROADCAST_RECV] Leaving function...\n");
    return;
  }

  // Check number of hops
  if (areq_recv->hops == 0)
  {
    printf("  [BROADCAST_RECV] Number of hops == 0, discard message and not rebroadcast!\n");
    printf("[BROADCAST_RECV] Leaving function...\n");
    return;
  }

  // Chek if message has been processed before
  for(areq_mem = list_head(list_areq); areq_mem != NULL; areq_mem = areq_mem->next)
  {
    if(linkaddr_cmp(&areq_mem->originators_address, &areq_recv->originators_address))
    {
      if(linkaddr_cmp(&areq_mem->requested_address, &areq_recv->requested_address))
      {
        // Discard message because it's repeated
        printf("  [BROADCAST_RECV] Message has been processed before, reject and not rebroadcast!!!.\n");
        printf("[BROADCAST_RECV] Leaving function...\n");
        return;
      }
    }
  }

  // Message it's not repeated, the hop number is greater than zero and the
  // tentative address is not the same as our address, so save the messages
  // in memory, in reverse route and rebroadcast!!

  // Saving to memory
  printf("  [BROADCAST_RECV] Add AREQ message to memory...\n");
  areq_mem = memb_alloc(&mem_areq);
  if (areq_mem != NULL)
  {
    linkaddr_copy(&areq_mem->originators_address, &areq_recv->originators_address);
    linkaddr_copy(&areq_mem->requested_address, &areq_recv->requested_address);
    list_add(list_areq, areq_mem);
    ctimer_set(&areq_mem->ctimer, REVERSE_ROUTE_LIFETIME, areq_remove, areq_mem);
  }else
  {
    printf("  [BROADCAST_RECV] ALLOCATE MEMORY ERROR\n");
  }

  // Saving to reverse route memory
  printf("  [BROADCAST_RECV] Add reverse route to memory\n");
  rra = memb_alloc(&mem_areq_reverse);
  if (rra != NULL)
  {
    linkaddr_copy(&rra->originators_address, &areq_recv->originators_address);
    linkaddr_copy(&rra->next_hop, from);
    list_add(list_areq_reverse_route, rra);
    ctimer_set(&rra->ctimer, REVERSE_ROUTE_LIFETIME, reverse_route_areq_remove, rra);
  }else
  {
    printf("  [BROADCAST_RECV] ALLOCATE MEMORY ERROR\n");
  }

  // Rebroadcast
  printf("  [BROADCAST_RECV] Rebroadcasting the message to all neighbours!\n");
  areq_recv->hops--;
  broadcast_send(&broadcast);

  printf("[BROADCAST_RECV] Leaving function...\n");
  return;
}
static const struct broadcast_callbacks broadcast_call = {broadcast_recv};
/*-------------------------- MULTIHOP FUNCTIONS  -----------------------------*/
/*
 * This function is called at the final recepient of the message.
 */
static void recv(struct multihop_conn *c, const linkaddr_t *sender,
                 const linkaddr_t *prevhop, uint8_t hops)
{
  printf("[MULTIHOP-RECV] Entering function...\n");

  printf("  [MULTIHOP-RECV] AREP message received: %s, change to NO_ADDRESS_STATE\n",
        (char *)packetbuf_dataptr());
  actual_state = NO_ADDRESS_STATE;

  printf("[MULTIHOP-RECV] Leaving function...\n");

  return;
}

/*
 * This function is called to forward a packet. The function picks a
 * the neighbor from the neighbor list and returns its address. The
 * multihop layer sends the packet to this address. If no neighbor is
 * found, the function returns NULL to signal to the multihop layer
 * that the packet should be dropped.
 */
static linkaddr_t * forward(struct multihop_conn *c,
                            const linkaddr_t *originator, const linkaddr_t *dest,
	                          const linkaddr_t *prevhop, uint8_t hops)
{
  printf("[MULTIHOP-FORWARD] Entering function...\n");
  printf("  [MULTIHOP-FORWARD] Multihop destination: %d.%d\n", dest->u8[0], dest->u8[1]);


  struct reverse_route_areq *rra;

  // Check if we are in NORMAL_STATE
  if(actual_state != NORMAL_STATE)
  {
    printf("  [MULTIHOP-FORWARD] AREP message not resend, actual state is not NORMAL_STATE\n");
    printf("[MULTIHOP-FORWARD] Leaving function...\n");
    return NULL;
  }

  // Searching on the list for the receiver and its next hop
  for(rra = list_head(list_areq_reverse_route); rra != NULL; rra = rra->next)
  {
    if(linkaddr_cmp(&rra->originators_address, dest))
    {
      printf("  [MULTIHOP-FORWARD] Sending AREP, next hop: %d.%d\n",
            rra->next_hop.u8[0],
            rra->next_hop.u8[1]);
      printf("[MULTIHOP-FORWARD] Leaving function...\n");
      return &rra->next_hop;
    }
  }

  printf("  [MULTIHOP-FORWARD] Neigbour not found on reverse route list\n");
  printf("[MULTIHOP-FORWARD] Leaving function...\n");
  return NULL;
}
static const struct multihop_callbacks multihop_call = {recv, forward};
/*----------------------------- MAIN PROCESS ---------------------------------*/
PROCESS_THREAD(qdad_process, ev, data)
{
  // Variables must be static!!
  static struct etimer et;
  static struct areq msg_areq;
  static linkaddr_t tentative_address;
  static int times_forward_areq = 0;

  PROCESS_EXITHANDLER(broadcast_close(&broadcast);)
  PROCESS_EXITHANDLER(multihop_close(&multihop);)

  PROCESS_BEGIN();

  // Initialize the memory for the neighbor table entries
  memb_init(&mem_areq);
  memb_init(&mem_areq_reverse);

  // Initialize the list used for the neighbor table
  list_init(list_areq);
  list_init(list_areq_reverse_route);

  // Open broadcast on channel 129 (from example-broadcast)
  broadcast_open(&broadcast, 129, &broadcast_call);
  // Open a multihop connection on Rime channel 135 (from example-multihop)
  multihop_open(&multihop, 135, &multihop_call);


  while(1)
  {
    printf ("[MAIN] Mote address: %d.%d\n", linkaddr_node_addr.u8[0],
                                            linkaddr_node_addr.u8[1]);

    if (actual_state == NO_ADDRESS_STATE)
    {
      printf("[MAIN] Actual state is: NO_ADDRESS_STATE\n");

      // Restart variable times_forward_areq
      times_forward_areq = 0;

      // Set tentative address
      printf("  [MAIN - NO_ADDRESS_STATE] Selecting tentative address...\n");
      tentative_address.u8[0] = random_rand();
      tentative_address.u8[1] = random_rand();
      printf("  [MAIN - NO_ADDRESS_STATE] Tentative address: %d.%d\n",
            tentative_address.u8[0],
            tentative_address.u8[1]);

      // Set random t_start [0 < t_start < MAX_T_START]
      int t_start = random_rand() % MAX_T_START;
      printf("  [MAIN - NO_ADDRESS_STATE] Random t_START: %d s.\n", t_start);
      etimer_set(&et, CLOCK_SECOND*t_start);
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

      // Copy to originators_address the address assigned by cooja
      linkaddr_copy(&msg_areq.originators_address, &linkaddr_node_addr);

      // Copy to requested_address the tentative address
      linkaddr_copy(&msg_areq.requested_address, &tentative_address);

      // Set the maximun number of hops
      msg_areq.hops = MAX_HOPS;

      // Next state of FSM
      actual_state = ADVERTISING_STATE;
    }else if(actual_state == ADVERTISING_STATE)
    {
      printf("[MAIN] Actual state is: ADVERTISING_STATE\n");

      // Increment counter of times broadcast AREQ package
      times_forward_areq++;
      if (times_forward_areq <= ADDRESS_RETRIES)
      {
        printf("  [MAIN - ADVERTISING_STATE] Broadcasting AREQ...\n");
        printf("  [MAIN - ADVERTISING_STATE] Times forwarding AREQ: %d\n",
              times_forward_areq);

        // This function clears the packetbuf and resets all internal state
        // pointers (header size, header pointer, external data pointer).
        packetbuf_clear();
        packetbuf_copyfrom (&msg_areq , sizeof(struct areq));

        // Send data using broadcast
        broadcast_send(&broadcast);

        // Wait until expire the timer associated
        etimer_set(&et, ADDRESS_DISCOVERY);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
      }else
      {
        printf("  [MAIN - ADVERTISING_STATE] Assigned new address!\n");

        // Set the new address
        linkaddr_set_node_addr(&tentative_address);

        // Next state of FSM
        actual_state = NORMAL_STATE;
      }
    }else if(actual_state == NORMAL_STATE)
    {
      printf("[MAIN] Actual state is: NORMAL_STATE\n");

      // Timer for not run continuously (wait one sec)
      etimer_set(&et, CLOCK_SECOND);
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
    }else
    {
      printf("[MAIN] State not correct, returning to NO_ADDRESS_STATE now...\n");

      // Next state of FSM
      actual_state = NO_ADDRESS_STATE;
    }
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
