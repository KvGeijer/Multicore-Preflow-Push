#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <limits.h>

#define COLLECT_STATS 0	/* enable/disable exit prints of stats as well as their collection */
#define PRINT		0	/* enable/disable prints. */
#define NUM_THREADS 20

#define B_MAX		16
#define B_MIN		1
#define B_IDLE_REQ	2
#define B_INTERVAL	200

#define FORSETE		0
#define NDEBUG

#include <assert.h>

#if COLLECT_STATS
#define count_stat(x) 	(x += 1);
#else
#define count_stat(x)	
#endif

#ifdef NDEBUG
#undef assert
#define assert(...)
#endif

#if PRINT
pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;

#define pr(...)		do {	/*pthread_mutex_lock(&print_lock); */		\
							printf(__VA_ARGS__);		\
							/*pthread_mutex_unlock(&print_lock);*/	\
					} while (0)
#else
#define pr(...)
#endif

#define MIN(a,b)	(((a)<=(b))?(a):(b))

typedef struct graph_t	graph_t;
typedef struct node_t	node_t;
typedef struct edge_t	edge_t;
typedef struct list_t	list_t;
typedef struct locked_node_list_t	locked_node_list_t;
typedef struct thread_t thread_t;
typedef struct init_info_t init_info_t;
typedef struct stats_t stats_t;
typedef struct static_graph_t static_graph_t;

// TODO: toggle for this?
typedef struct xedge_t	xedge_t;
struct xedge_t {
	int32_t		u;	/* one of the two nodes.	*/
	int32_t		v;	/* the other. 			*/
	int32_t		c;	/* capacity.			*/
};

struct stats_t {
	int discharges;
	int central_pops;
	int pushes;
	int nonsaturated_pushes;
	int relabels;
};

struct thread_t {
	short thread_id;
	node_t*		in;		/* Inqueue for nect processing.		*/
	node_t*		out;	/* Outqueue to either push to global to replace inqueue	(Andersson and Setubal 1995)*/
	int 	nbr_out;	/* How many nodes are in the out queue? */

	atomic_flag cont;
	graph_t* g;

	thread_t** threads;

#if COLLECT_STATS
	stats_t stats;
#endif
};

struct init_info_t {
	pthread_barrier_t* barrier;
	thread_t** threads;
	short thread_id; 
};

struct list_t {
	edge_t*		edge;
	list_t*		next;
};

struct locked_node_list_t {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	node_t* u;
	int size;
	long discharges;		/* The number of node discharges made, used as heuristic for global relabeling and changing b */
	short waiting;
};

struct node_t {
	pthread_mutex_t mutex; 	/* processing lock */
	list_t* 	progress;	/* To what edge we got last time processing this node */
	list_t*		edge;	/* adjacency list.		*/
	node_t* 	next;	/* For forming linked lists, should probaby be arrays of some sort */
	atomic_int	h;		/* height.			*/
	int		e;			/* excess flow.			*/
	int 	lowest_neigh;	/* Height of the lowest neighbour */
};

struct edge_t {
	node_t*		u;	/* one of the two nodes.	*/
	node_t*		v;	/* the other. 			*/
	int		f;	/* flow > 0 if from u to v.	*/
	int		c;	/* capacity.			*/
};

struct graph_t {
	atomic_int 	b; 	/* How much to fetch and push at a time when accessing queues */	//TODO: Move into threads somehow for caches
	int		n;	/* nodes.			*/
	int		m;	/* edges.			*/
	node_t*		v;	/* array of n nodes.		*/
	edge_t*		e;	/* array of m edges.		*/
	node_t*		s;	/* source.			*/
	node_t*		t;	/* sink.			*/

	locked_node_list_t excess;	/* nodes with e > 0 except s,t.	*/
};

struct static_graph_t {
	int n;
	int m;

	node_t* v;
	edge_t* e;
	list_t* links;

	thread_t** threads;
	pthread_barrier_t barrier;
};

static char* progname;



#if PRINT
static int id(graph_t* g, node_t* v)
{
	/* return the u index for v.
	 * 
	 * what happens is a subtract instruction followed by a
	 * divide by the size of the array element.
	 *
	 */

	return v - g->v;
}
#endif

// function definitions
static void* run(void* arg);
static void try_push(node_t* u, node_t* v, edge_t* e, graph_t* g, thread_t* thread);
static void destroy_graph(graph_t* g);

void error(const char* fmt, ...)
{
	/* print error message and exit. 
	 *
	 * it can be used as printf with formatting commands such as:
	 *
	 *	error("height is negative %d", v->h);
	 */

	va_list		ap;
	char		buf[BUFSIZ];

	va_start(ap, fmt);
	vsprintf(buf, fmt, ap);

	if (progname != NULL)
		fprintf(stderr, "%s: ", progname);

	fprintf(stderr, "error: %s\n", buf);
	exit(1);
}

static int next_int()
{
        int     x;
        int     c;

	/* this is like Java's nextInt to get the next integer. */

	x = 0;
        while (isdigit(c = getchar()))
                x = 10 * x + c - '0';

        return x;
}

static void* xmalloc(size_t s)
{
	void*		p;

	/* allocate s bytes from the heap and check that there was
	 * memory for our request.
	 *
	 */

	p = malloc(s);

	if (p == NULL)
		error("out of memory: malloc(%zu) failed", s);

	return p;
}

static void* xcalloc(size_t n, size_t s)
{
	void*		p;

	p = xmalloc(n * s);

	/* memset sets everything (in this case) to 0. */
	memset(p, 0, n * s);

	return p;
}

static void print_stats(thread_t* thread)
{
#if (COLLECT_STATS)

	printf("@%d: exiting, nodes: %d, central pops: %d, pushes: %d, nonsaturated pushes: %d, relabels: %d\n", 
		thread->thread_id, thread->stats.discharges, thread->stats.central_pops,
		thread->stats.pushes, thread->stats.nonsaturated_pushes, thread->stats.relabels);
#endif
}

static void add_edge(node_t* u, edge_t* e, list_t* p)
{

	/* allocate memory for a list link and put it first
	 * in the adjacency list of u.
	 *
	 */

	assert(u->edge != p);
	p->edge = e;
	p->next = u->edge;
	u->edge = p;
}

static void connect(node_t* u, node_t* v, int c, edge_t* e, list_t* link1, list_t* link2)
{
	/* connect two nodes by putting a shared (same object)
	 * in their adjacency lists.
	 *
	 */

	e->u = u;
	e->v = v;
	e->c = c;

	add_edge(u, e, link1);
	add_edge(v, e, link2);
}

static void init_lockedList(locked_node_list_t* list)
{

	//TODO: Assign statically? Remember to still reset values xD

	pthread_mutex_init(&list->mutex, NULL);
	pthread_cond_init(&list->cond, NULL);

	list->u = NULL;
	list->size = 0;
	list->waiting = 0;
	list->discharges = 0;
}

static void init_edges_forsete(graph_t* g, xedge_t* e, static_graph_t* stat_g)
{
	node_t *u;
	node_t *v;
	int a;
	int b;
	int c;
	int i;

	for (i = 0; i < g->m; i += 1) {
		a = e[i].u;
		b = e[i].v;
		c = e[i].c;
		u = &g->v[a];
		v = &g->v[b];
		connect(u, v, c, g->e+i, stat_g->links+(i*2), stat_g->links+(i*2) + 1);
	}
}

static void init_edges_normal(graph_t* g, static_graph_t* stat_g)
{
	node_t *u;
	node_t *v;
	int a;
	int b;
	int c;
	int i;

	for (i = 0; i < g->m; i += 1) {
		a = next_int();
		b = next_int();
		c = next_int();
		u = &g->v[a];
		v = &g->v[b];
		connect(u, v, c, g->e+i, stat_g->links+(i*2), stat_g->links+(i*2) + 1);
	}

	fclose(stdin);

}

static void init_threads(thread_t** threads, pthread_barrier_t* barrier)
{
	/**
	 * Creates and starts the pthreads to run the program.
	 * 
	 * Makes sure the threads have linked their structs 
	 * before proceeding using the barrier. The memory for
	 * the threads is just yeeted into the abyss. Could
	 * be stored in the static graph if we need them for
	 * something later.
	 */

	int i;

	init_info_t init_infos[NUM_THREADS];
	pthread_t* pthreads;	//MEMORY LEAK

	pthreads = xmalloc(NUM_THREADS * sizeof(pthread_t));

	for (i = 0; i < NUM_THREADS; i++) {
		init_infos[i].thread_id = i;
		init_infos[i].barrier = barrier;
		init_infos[i].threads = threads;

		pthread_create(&pthreads[i], NULL, &run, &init_infos[i]);
	}
	
	pr("MAIN: Waiting from thread links\n");
	pthread_barrier_wait(barrier);

}

static void init_static_graph(static_graph_t* stat_g, int n, int m)
{
	int i;

	// TODO: allocate more memory than immediately needed
	stat_g->n = n;
	stat_g->m = m;

	stat_g->v = xcalloc(n, sizeof(node_t));
	stat_g->e = xcalloc(m, sizeof(edge_t));
	stat_g->links = xcalloc(m * 2, sizeof(list_t));

	stat_g->threads = xcalloc(NUM_THREADS, sizeof(thread_t*));
	pthread_barrier_init(&stat_g->barrier, NULL, NUM_THREADS + 1);

	init_threads(stat_g->threads, &stat_g->barrier);

	for (i = 0; i < n; i++) {
		pthread_mutex_init(&stat_g->v[i].mutex, NULL);
	}

}

static void update_static_graph(static_graph_t* stat_g, int n, int m)
{
	int i;
	int j;

	// Nodes
	if (n > stat_g->n){
		stat_g->v = realloc(stat_g->v, n * sizeof(node_t));

		for (i = stat_g->n; i < n; i++) {
			pthread_mutex_init(&stat_g->v[i].mutex, NULL);
		}

		stat_g->n = n;

	} 

	// Edges
	if (m > stat_g->m){
		stat_g->e = realloc(stat_g->e, m * sizeof(edge_t));
		stat_g->links = realloc(stat_g->links, m * 2 * sizeof(edge_t));

		stat_g->m = m;
	}

}

static void link_and_reset_graph(graph_t* g, static_graph_t* stat_g, int s, int t)
{

	// TODO: Move this to destroy_graph? Then we can more accurately de-initialize what was used
	g->v = stat_g->v;
	for (int i = 1; i < g->n; i += 1)
	{
		g->v[i].e = 0;
		atomic_store_explicit(&g->v[i].h, 0, memory_order_relaxed);
		g->v[i].edge = NULL;
		g->v[i].progress = NULL;
		g->v[i].lowest_neigh = 0;
		g->v[i].next = NULL;
	}
	g->v[0].e = 0;
	g->v[0].edge = NULL;


	g->e = stat_g->e;
	for (int j = 0; j < g->m; j += 1)
	{
		g->e[j].f = 0;
	}

	g->s = &g->v[s];
	g->t = &g->v[t];
	atomic_store_explicit(&g->s->h, g->n, memory_order_release);

}

static void adjust_sink_height(graph_t* g)
{
	list_t* p;
	int max_cap;

	max_cap = 0;
	p = g->t->edge;
	while (p != NULL) {
		max_cap += p->edge->c;
		p = p->next;
	}

	g->t->e = -max_cap;
	atomic_store_explicit(&g->t->h, -max_cap, memory_order_relaxed);	
	//Just to save space, could alloc to g instead

}

static void prepare_threads(graph_t* g, thread_t** threads)
{
	thread_t* thread;
	for (int i = 0; i < NUM_THREADS; i += 1){
		thread = threads[i];
		thread->g = g;
	}
}

static pthread_barrier_t* init_graph(graph_t* g, int n, int m, int s, int t, xedge_t* e)
{
	/**
	 * Initializes the graph, as well as other structures used
	 * to solve it. Keeps track of the things with static life
	 * span such as the threads and barrier.
	 * 
	 * Returns the barrier so that we can use it to wait in
	 * the answer in preflow.
	 */

	static static_graph_t* stat_g;

	g->n = n;
	g->m = m;
	g->b = B_MAX;

	if (stat_g != NULL) 
	{
		update_static_graph(stat_g, n, m);
	}
	else 
	{
		stat_g = xcalloc(1, sizeof(static_graph_t));
		init_static_graph(stat_g, n, m);
	}

	link_and_reset_graph(g, stat_g, s, t);
	// TODO: Use OpenMP to divide into sections and parallelize loops

	init_lockedList(&g->excess);

#if (FORSETE)
	init_edges_forsete(g, e, stat_g);
#else 
	init_edges_normal(g, stat_g);
#endif

	adjust_sink_height(g);

	prepare_threads(g, stat_g->threads);

	return &stat_g->barrier;
}

static void clear_flags(thread_t** threads)
{
	int i;
	pr("CLEARING FLAGS\n");

	for (i = 0; i < NUM_THREADS; i += 1)
	{
		// TODO: Can this be relaxed?
		atomic_flag_clear_explicit(&threads[i]->cont, memory_order_seq_cst);
	}
}

static void enter_outqueue(node_t* v, thread_t* thread)
{
	/* Enter the node v into the private outqueue
	 */

	if (thread->out != NULL) {
		v->next = thread->out->next;
		thread->out->next = v;
	} else {
		v->next = v;
	}

	thread->out = v;
	thread->nbr_out += 1;

}

static void check_b_rules(graph_t* g, thread_t* thread)
{
	/* Only get here occasionally and then potentially change the b if certain conditions satisfied */

	//TODO: Fix b
	int b = atomic_load_explicit(&g->b, memory_order_relaxed);

	if (b > B_MIN && g->excess.waiting >= B_IDLE_REQ) 
	{
		atomic_store_explicit(&g->b, b / 2, memory_order_release);	// Release needed?
	}
	else if (b < B_MAX && (NUM_THREADS - g->excess.waiting) + g->excess.size / b > 1.5 * NUM_THREADS)	// TODO: Fine tune
	{
		atomic_store_explicit(&g->b, b * 2, memory_order_release);	// Can swap mul/div for shift
	}

}

static void publish_outqueue(graph_t* g, thread_t* thread)
{
	/* Append the nodes in the outqueue to the global excess list.
	 */

	node_t* first;

	pthread_mutex_lock(&g->excess.mutex);

	g->excess.discharges += thread->nbr_out;

	if (g->excess.discharges % 200 > thread->nbr_out) {
		check_b_rules(g, thread);
	}

	if (g->excess.u != NULL) {
		first = g->excess.u->next;

		g->excess.u->next = thread->out->next;
		thread->out->next = first;
		
		
	} else {
		// TODO: Maybe signal here?
	}

	g->excess.u = thread->out;

	pthread_mutex_unlock(&g->excess.mutex);
	pthread_cond_signal(&g->excess.cond);

	thread->nbr_out = 0;
	thread->out = NULL;

}

static void enter_excess(graph_t* g, node_t* v, thread_t* thread)
{
	/* Put v into the outqueue and maybe publish it
	 */

	assert(v->e > 0);

	// TODO: Remove v == g->t? As it now has so much negative preflow :D Also g->s should be removed
	if (v == g->t || v == g->s || atomic_load_explicit(&v->h, memory_order_relaxed) >= g->n) {
		return ;
	}

	pr("@%d: entering node into outqueue, node = %d, nbr out = %d\n", thread->thread_id, id(g, v), thread->nbr_out);
	enter_outqueue(v, thread);

	if (thread->nbr_out >= g->b) {
		assert(thread->nbr_out == g->b);

		pr("@%d: publishing outqueue, nbr out = %d\n", thread->thread_id, thread->nbr_out);
		publish_outqueue(g, thread);
	}
	
}

static node_t* pop_inqueue(thread_t* thread)
{
	/* Pop a node from the private inqueue, It should be in the form of a stack. */

	node_t* u;

	u = thread->in;
	thread->in = thread->in->next;

	assert(u != NULL);
	return u;
}

static void move_out_to_in(thread_t* thread)
{
	assert(thread->out !=NULL);

	thread->in = thread->out->next;

	thread->out->next = NULL;
	thread->out = NULL;
	thread->nbr_out = 0;

}

static node_t* refill_inqueue(thread_t* thread)
{
	/* take any u from the set of nodes with excess preflow
	 * and for simplicity we always take the first.
	 * 
	 */

	node_t*		u;
	node_t*		temp;
	graph_t* 	g;
	int 		i;

	g = thread->g;
	assert(thread->in == NULL);

	pthread_mutex_lock(&g->excess.mutex);

	// TODO: FIX
	if (g->excess.u == NULL) 
	{

		// If global queue is empty we instead try to take from our outqueue.
		if (thread->out != NULL) {
			pthread_mutex_unlock(&g->excess.mutex);

			pr("@%d: moving out to in\n", thread->thread_id);
			move_out_to_in(thread);
			pr("@%d: moved out to in\n", thread->thread_id);
			return pop_inqueue(thread);
		}

		g->excess.waiting += 1;
		while (g->excess.u == NULL)
		{

			if (g->excess.waiting == NUM_THREADS || 
				!atomic_flag_test_and_set_explicit(&thread->cont, memory_order_relaxed)) 
			{
				pthread_mutex_unlock(&g->excess.mutex);
				pthread_cond_signal(&g->excess.cond);	// TODO: only signal when clearning flags?

				pr("@%d: returning NULL, waiting = %d\n", thread->thread_id, g->excess.waiting);
				return NULL;
			}
			else if (g->excess.waiting < NUM_THREADS)
			{
				// normal case
				// TODO: Try busy wait
				assert(g->excess.waiting <= NUM_THREADS);
				pr("@%d: waiting, waiting = %d\n", thread->thread_id, g->excess.waiting);

				pthread_cond_wait(&g->excess.cond, &g->excess.mutex);
				
			}
		}
		g->excess.waiting -= 1;
	}

	if (g->excess.size > g->b) {
		
		temp = g->excess.u;
		u = g->excess.u->next;

		for (i = 0; i < g->b; i += 1) {
			// Move temp forward to the last node to fetch
			temp = temp->next;
			assert(temp != u);
		}

		g->excess.u->next = temp->next;
		temp->next = NULL; 	// Make it a stack representing a portion of the FIFO queue

	}
	else {
		// If we fetched the last elements in the queue we have to empty it properly
		u = g->excess.u->next;
		g->excess.u->next = NULL;	// Make it a stack
		g->excess.u = NULL;
	}

	pthread_mutex_unlock(&g->excess.mutex);

	count_stat(thread->stats.central_pops);
	thread->in = u->next;
	return u;
}

static void push(graph_t* g, node_t* u, node_t* v, edge_t* e, int flow, thread_t* thread)
{
	/* Assumes you hold all necessary locks */

	if (u == e->u) {
		e->f += flow;
	} else {
		e->f -= flow;
	}

	pr("@%d: pushing from %d to %d: f = %d, c = %d, d = %d\n", 
		thread->thread_id, id(g, u), id(g, v), e->f, e->c, flow);
	count_stat(thread->stats.pushes);

	u->e -= flow;
	v->e += flow;

	/* the following are always true. */

#if COLLECT_STATS
	if (abs(e->f) != e->c){
		count_stat(thread->stats.nonsaturated_pushes);
	}
#endif

	assert(flow > 0);
	assert(u->e >= 0 || u == g->s);
	assert(abs(e->f) <= e->c);

	if (v->e == flow) {

		/* since v has d excess now it had zero before and
		 * can now push.
		 */

		enter_excess(g, v, thread);
	} 
	else if (v->e == 0) {
		// Must be the sink!
		pr("@%d: Sink completely filled\n", thread->thread_id);
		assert(v == g->t);
		clear_flags(thread->threads);	//TODO: More?

		pthread_cond_broadcast(&g->excess.cond);	// ERROR: maybe wonky
	}
}

static void try_relabel(node_t* u, thread_t* thread)
{
	/* Assumes we hold the lock for u. Only relabels if lower than all neighbours  */

	int new_h;

	new_h = u->lowest_neigh + 1;
	if (new_h > atomic_load_explicit(&u->h, memory_order_relaxed)) {
		atomic_store_explicit(&u->h, new_h, memory_order_release);

		pr("@%d: relabel %d now h = %d\n", thread->thread_id, id(thread->g, u), u->h);
		count_stat(thread->stats.relabels);
		assert(new_h < INT_MAX);
	}
	
	// Reset
	u->lowest_neigh = INT_MAX;
}

static node_t* other(node_t* u, edge_t* e)
{
	if (u == e->u)
		return e->v;
	else
		return e->u;
}

static void source_pushes(graph_t* g, thread_t* thread)
{
	list_t* p;
	node_t* s;
	node_t* v;
	edge_t* e;
	int n;

	count_stat(thread->stats.discharges);

	pr("@%d: Starting source pushes\n", thread->thread_id);

	s = g->s;
	n = s->h;

	pthread_mutex_lock(&s->mutex);
	
	p = s->edge;

	// Try to push to all neighbours.
	while(p != NULL) {

		e = p->edge;
		p = p->next;
		v = other(s, e);

		// Hack to make s have "infinite preflow"
		s->e += e->c;

		/* helper func tp check if we can push and maybe do it */
		pr("@%d: Trying source push to %d\n", thread->thread_id, id(g, v));
		try_push(s, v, e, g, thread);
		
	}

	pthread_mutex_unlock(&s->mutex);

}


static int xpreflow(graph_t* g, pthread_barrier_t* barrier)
{
	/**
	 * Releases the threads and waits for them to finish the algorithm.
	 */

	int f;

	pthread_barrier_wait(barrier);
	pr("MAIN: Threads released!\n");
	pthread_barrier_wait(barrier);
	pr("MAIN: Main released!\n");

	f = g->t->e - g->t->h;	// ERROR: reset up from negative preflow at start
	return f;

}

static int can_push(node_t* u, node_t* v, edge_t* e)
{
	/* Returns how much u can push to v. Assumes you have observed if u has changed flow over e */

	int flow;
	int uh;
	int vh;

	assert(u->e > 0);

	if (u == e->u) {
		flow = MIN(u->e, e->c - e->f);
	} else {
		flow = MIN(u->e, e->c + e->f);
	}

	// TODO: Pre-compute u_h and pass around? Should be faster
	// ERROR: Look out for memory order problems
	if (flow)
	{
		uh = atomic_load_explicit(&u->h, memory_order_relaxed);
		vh = atomic_load_explicit(&v->h, memory_order_consume);	// TODO: try relaxed
		
		if  (uh > vh) {
			return flow;
		}
		else if (vh < u->lowest_neigh) {
			u->lowest_neigh = vh;
		}
		
	}
	
	return 0;
	

}

static void try_push(node_t* u, node_t* v, edge_t* e, graph_t* g, thread_t* thread)
{
	/* Tries pushing from u to v, but only if possible.
	*/

	int flow;

	//TODO: Make height not SC
	flow = can_push(u, v, e);

	if (flow)
	{
		// First aquire the locks in the correct order
		if (v < u) {
			pthread_mutex_unlock(&u->mutex);
			pthread_mutex_lock(&v->mutex);
			pthread_mutex_lock(&u->mutex);
		} else {
			pthread_mutex_lock(&v->mutex);
		}

		// Push if we can
		push(g, u, v, e, flow, thread);

		pthread_mutex_unlock(&v->mutex);
	}
	else
	{
		pr("@%d: aborted push from %d to %d\n", thread->thread_id, id(g, u), id(g, v));
	}
}

static void discharge(node_t* u, graph_t* g, thread_t* thread)
{
	/* Discharges a single node.
	* 
	* Now no longer guarantees to push away all flow.
	*
	* First checks if it should relabel, then tries to push
	* to as many neighbours as possible before possibly placing
	* the node in the outqueue if it has any flow left.
	*/

	list_t* p;
	list_t* prog;
	edge_t* e;
	node_t *v;

	pthread_mutex_lock(&u->mutex);

	// If saved progress we should continue and not try to relabel.
	if (u->progress == NULL) {
		try_relabel(u, thread);
		p = u->edge;
	} 
	else {
		// Only if we ran out of excess preflow, not if we mismatch waves.
		p = u->progress;
	}


	// Try to push to all neighbours.
	while(p != NULL && u->e > 0) {
		prog = p;

		e = p->edge;
		p = p->next;
		v = other(u, e);

		/* helper func to check if we can push and maybe do it */
		try_push(u, v, e, g, thread);
		
	}

	if (u->e > 0) 
	{	
		// TODO: Could do after unlocking the node
		u->progress = NULL;
		enter_excess(g, u, thread);
	} 
	else if (p != NULL) 	// Try to remove
	{	
		u->progress = prog;
	}
	
	pthread_mutex_unlock(&u->mutex);

}


static node_t* leave_excess(graph_t* g, thread_t* thread)
{
	// TODO: change from stack?
	node_t* u;

	if (atomic_flag_test_and_set_explicit(&thread->cont, memory_order_relaxed))
	{
		if (thread->in != NULL) 
		{
			u = pop_inqueue(thread);

			pr("@%d: getting private node: %d\n", thread->thread_id, id(g, u));
		}
		else 
		{
			u = refill_inqueue(thread);

			pr("@%d: refilling inqueue and getting: %d\n", thread->thread_id, id(g, u));
		}
	} 
	else {
		return NULL;
	}

	return u;
}

static void init_thread(thread_t* thread, init_info_t* init, pthread_barrier_t* barrier)
{
	*thread = (thread_t){	.thread_id = init->thread_id, 	//TODO: Remove from struct?
						.in = NULL, 
						.out = NULL, 
						.nbr_out = 0,
						.g = NULL,
						.cont = 1,			//ERROR: Does this work?
						.threads = init->threads,
#if COLLECT_STATS
						.stats = {0},
#endif
						};
	
	pr("@%d: linking thread\n", thread->thread_id);

	init->threads[init->thread_id] = thread;
	pthread_barrier_wait(barrier);

	pr("@%d: starting run\n", thread->thread_id);
}

static void reset_thread(thread_t* thread)
{
	thread->nbr_out = 0;
	thread->in = NULL;
	thread->out = NULL;
}

static void* run(void* arg)
{
	/* Run method for the threads */

	init_info_t* init = arg;
	node_t* u;
	pthread_barrier_t* barrier;
	thread_t thread;
	graph_t* g;

	barrier = init->barrier;
	init_thread(&thread, init, barrier);

	while(1){

		// Synchronize the threads at the start and end of each graph!
		pthread_barrier_wait(barrier);
		g = thread.g;	// TODO: Just use reference in thread?

		pr("@%d: Starting a new graph with n = %d, m = %d\n", thread.thread_id, g->n, g->m);

		if (thread.thread_id == 0) {
			source_pushes(g, &thread);
		}

		while((u = leave_excess(g, &thread)) != NULL)
		{
			pr("@%d: selected u = %d with h = %d and e = %d\n", thread.thread_id, id(g, u), u->h, u->e);

			discharge(u, g, &thread);
			count_stat(thread.stats.discharges);
		}

		pr("@%d: Finished a graph\n", thread.thread_id);
		print_stats(&thread);
		pthread_barrier_wait(barrier);
		reset_thread(&thread);

	}

	return NULL;
}

static void destroy_graph(graph_t* g)
{
	// Very sad now that almost nothing is free'd	

	pthread_cond_destroy(&g->excess.cond);
	pthread_mutex_destroy(&g->excess.mutex);

}

int preflow(int n, int m, int s, int t, xedge_t* e)
{
	graph_t g;
	int f;
	pthread_barrier_t* barrier;

	barrier = init_graph(&g, n, m, s, t, e);

	f = xpreflow(&g, barrier);

	destroy_graph(&g);

	return f;

}

#if !(FORSETE)
int main(int argc, char* argv[])
{
	int		f;	/* output from preflow.		*/
	int		n;	/* number of nodes.		*/
	int		m;	/* number of edges.		*/

	progname = argv[0];	/* name is a string in argv[0]. */

	n = next_int();
	m = next_int();

	/* skip C and P from the 6railwayplanning lab in EDAF05 */
	next_int();
	next_int();

	f = preflow(n, m, 0, n-1, NULL);

	printf("f = %d\n", f);

}
#endif

