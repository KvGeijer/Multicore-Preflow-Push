#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

#define COLLECT_STATS 0	/* enable/disable exit prints of stats as well as their collection */
#define PRINT		0	/* enable/disable prints. */
#define NUM_THREADS 20
#define LOCAL_QUEUE 2

/************** MEMORY CONSTANTS **************/

#define MEMORY_UPDATE		2
#define STARTING_ADJ		1500	// TODO

/**********************************************/

#define FORSETE		1
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
typedef struct adj_t 	adj_t;
typedef struct locked_node_list_t	locked_node_list_t;
typedef struct thread_t thread_t;
typedef struct init_info_t init_info_t;
typedef struct stats_t stats_t;
typedef struct static_graph_t static_graph_t;

typedef struct xedge_t	xedge_t;
struct xedge_t {
	int32_t		u;	/* one of the two nodes.	*/
	int32_t		v;	/* the other. 			*/
	int32_t		c;	/* capacity.			*/
};

struct stats_t {
	int nodes_processed;
	int central_pops;
	int pushes;
	int nonsaturated_pushes;
	int relabels;
};

struct thread_t {
	short thread_id;
	short nbr_nodes;
	node_t* next;	/* Private work queue */

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

struct locked_node_list_t {
	short waiting;
	int size;
	node_t* u;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
};

struct node_t {
	atomic_int	h;	/* height.			*/
	int		e;	/* excess flow.			*/
	int 	nbr_edge;	/* The number of edges for the node */
	adj_t*		adj;	/* adjacency list.		*/
	node_t*		next;	/* with excess preflow.		*/
	pthread_mutex_t mutex; 	/* processing lock */
};

struct edge_t {	// TODO: Reuse xedge_t?!
	int		f;	/* flow > 0 if from u to v.	*/
	int		c;	/* capacity.			*/
};


struct adj_t {
	int v;		/* The index of the other node */ 
	int index;	/* The index of the edge in the larger edge array */ 
	short dir;	/* Forward or backward edge in relation to the node */
};

struct graph_t {
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

	int* alloced_adj;
	node_t* v;
	edge_t* e;

	thread_t** threads;	// TODO: Test changing to an array of pointers
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
static void try_push(node_t* u, node_t* v, edge_t* e, short dir, graph_t* g, thread_t* thread);
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
		thread->thread_id, thread->stats.nodes_processed, thread->stats.central_pops,
		thread->stats.pushes, thread->stats.nonsaturated_pushes, thread->stats.relabels);
#endif
}

static void init_lockedList(locked_node_list_t* list)
{
	pthread_mutex_init(&list->mutex, NULL);
	pthread_cond_init(&list->cond, NULL);

	list->u = NULL;
	list->size = 0;
	list->waiting = 0;
}

static inline void connect(node_t* u, int vi, int ei, int* static_cap, short forward)
{
	/* connect an edge with a node by initializing the adj object.
	 */

	adj_t* adj;

	if (u->nbr_edge >= *static_cap) {
		*static_cap *= MEMORY_UPDATE;
		u->adj = realloc(u->adj, *static_cap);
	}

	adj = &u->adj[u->nbr_edge];
	adj->v = vi;
	adj->index = ei;
	adj->dir = forward;

	u->nbr_edge += 1;
}

static void init_edges_forsete(graph_t* g, xedge_t* e, int* static_caps)
{
	node_t *u;
	node_t *v;
	int ui;
	int vi;
	int c;
	int i;

	for (i = 0; i < g->m; i += 1) {
		ui = e[i].u;
		vi = e[i].v;
		c = e[i].c;
		u = &g->v[ui];
		v = &g->v[vi];

		connect(u, vi, i, static_caps + ui, 1);
		connect(v, ui, i, static_caps + vi, 0);
		pr("Connecting u: %d == %d and v: %d == %d\n", ui, id(g, u), vi, id(g, v));
		g->e[i].c = c;
	}
}

static void init_edges_normal(graph_t* g, int* static_caps)
{
	node_t *u;
	node_t *v;
	int ui;
	int vi;
	int c;
	int i;

	for (i = 0; i < g->m; i += 1) {
		ui = next_int();
		vi = next_int();
		c = next_int();
		u = &g->v[ui];
		v = &g->v[vi];

		connect(u, vi, i, static_caps + ui, 1);
		connect(v, ui, i, static_caps + vi, 0);
		pr("Connecting u: %d == %d and v: %d == %d\n", ui, id(g, u), vi, id(g, v));
		g->e[i].c = c;
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
	int edges_per_node;
	int adj_per_node;

	// TODO: allocate more memory than immediately needed
	stat_g->n = n;
	stat_g->m = m;

	stat_g->v = xcalloc(n, sizeof(node_t));
	stat_g->e = xcalloc(m, sizeof(edge_t));

	stat_g->alloced_adj = xmalloc(m * sizeof(edge_t));

	for (i = 0; i < n; i += 1) {
		
		stat_g->v[i].adj = xcalloc(STARTING_ADJ, sizeof(adj_t*));	
		stat_g->alloced_adj[i] = STARTING_ADJ;
		pthread_mutex_init(&stat_g->v[i].mutex, NULL);
	}

	stat_g->threads = xcalloc(NUM_THREADS, sizeof(thread_t*));
	pthread_barrier_init(&stat_g->barrier, NULL, NUM_THREADS + 1);

	init_threads(stat_g->threads, &stat_g->barrier);

}

static void update_static_graph(static_graph_t* stat_g, int n, int m)
{
	int i;
	int j;

	// Nodes
	if (n > stat_g->n){
		stat_g->v = realloc(stat_g->v, n * sizeof(node_t));

		for (i = stat_g->n; i < n; i++) {
			stat_g->v[i].adj = xcalloc(STARTING_ADJ, sizeof(adj_t*));	
			stat_g->alloced_adj[i] = STARTING_ADJ;
			pthread_mutex_init(&stat_g->v[i].mutex, NULL);

		}

		stat_g->n = n;

	} 

	// Edges
	if (m > stat_g->m){
		stat_g->e = realloc(stat_g->e, m * sizeof(edge_t));

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
		g->v[i].next = NULL;
		g->v[i].nbr_edge = 0;
	}
	g->v[0].e = 0;
	g->v[0].nbr_edge = 0;


	g->e = stat_g->e;
	for (int j = 0; j < g->m; j += 1)
	{
		g->e[j].f = 0;
	}

	g->s = &g->v[s];
	g->t = &g->v[t];
	atomic_store_explicit(&g->s->h, g->n, memory_order_relaxed);

}

static void adjust_sink_height(graph_t* g)
{
	adj_t* adj;
	int max_cap;
	int i;

	adj = g->t->adj;
	max_cap = 0;
	for (i = 0; i < g->t->nbr_edge; i+= 1) {
		max_cap += g->e[adj[i].index].c;
	}

	g->t->e = -max_cap;
	atomic_store_explicit(&g->t->h, -max_cap, memory_order_relaxed);	//Just to save space, could alloc to g instead
	// ERROR: Check if this causes error, maybe not cleaned up right.

}

static void prepare_threads(graph_t* g, thread_t** threads)
{
	thread_t* thread;
	for (int i = 0; i < NUM_THREADS; i += 1){
		thread = threads[i];
		thread->g = g;
		// ERROR: Do we need more here? Is the flag already set?
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
	init_edges_forsete(g, e, stat_g->alloced_adj);
#else 
	init_edges_normal(g, stat_g->alloced_adj);
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

static void enter_private_excess(node_t* v, thread_t* thread)
{
	/* Enter the node v into the private work queue.
	 * 
	 * Atm just Lifo queue, but maybe Fifo better?
	 * 
	 * Really does not need you to hold the lock for v. But so fast...
	 */

	v->next = thread->next;
	thread->next = v;
	thread->nbr_nodes += 1;

}

static void enter_global_excess(graph_t* g, node_t* v)
{
	/* Enter the node v into the global work queue.
	 * 
	 * Atm just Lifo queue, but maybe Fifo better?
	 */

	pthread_mutex_lock(&g->excess.mutex);

	v->next = g->excess.u;
	g->excess.u = v;
	g->excess.size += 1;

	pthread_mutex_unlock(&g->excess.mutex);
	pthread_cond_signal(&g->excess.cond);

}

static void enter_excess(graph_t* g, node_t* v, thread_t* thread)
{
	/* Put v into a work queue depending on circumstance.
	 *
	 * Atm you hold the lock for v, but do you have to?
	 * 
	 */

	assert(v->e > 0);

	// TODO: Better to not read height here? Maybe load in advance? Better to have == sink/source in node thread?
	// TODO: Remove v == g->t? As it now has so much negative preflow :D
	if (v == g->t || v == g->s || atomic_load_explicit(&v->h, memory_order_relaxed) >= g->n) {
		return ;
	}

	if (thread->nbr_nodes < LOCAL_QUEUE)
	{
		pr("@%d: entering private excess, node = %d, nbr private = %d\n", 
			thread->thread_id, id(g, v), thread->nbr_nodes);

		enter_private_excess(v, thread);
	}
	else 
	{
		
		pr("@%d: entering global excess, node = %d, nbr private = %d\n", 
			thread->thread_id, id(g, v), thread->nbr_nodes);

		enter_global_excess(g, v);
	}
	
}

static node_t* leave_private_excess(thread_t* thread)
{
	node_t* u;

	u = thread->next;
	thread->next = u->next;
	thread->nbr_nodes -= 1;

	assert(u != NULL);
	return u;
}

static node_t* leave_global_excess(graph_t* g, thread_t* thread)
{
	node_t*		v;

	/* take any u from the set of nodes with excess preflow
	 * and for simplicity we always take the first.
	 * 
	 */

	pthread_mutex_lock(&g->excess.mutex);

	while (g->excess.u == NULL)
	{
		g->excess.waiting += 1;

		if (g->excess.waiting == NUM_THREADS || 
			!atomic_flag_test_and_set_explicit(&thread->cont, memory_order_relaxed)) 
		{
			pthread_mutex_unlock(&g->excess.mutex);
			pthread_cond_signal(&g->excess.cond);

			pr("@%d: returning NULL, waiting = %d\n", thread->thread_id, g->excess.waiting);
			return NULL;
		}
		else if (g->excess.waiting < NUM_THREADS)
		{
			// normal case
			assert(g->excess.waiting <= NUM_THREADS);
			pr("@%d: waiting, waiting = %d\n", thread->thread_id, g->excess.waiting);

			pthread_cond_wait(&g->excess.cond, &g->excess.mutex);
			g->excess.waiting -= 1;
		}
	}

	v = g->excess.u;

	g->excess.u = v->next;
	g->excess.size -= 1;

	pthread_mutex_unlock(&g->excess.mutex);

	count_stat(thread->stats.central_pops);
	return v;
}

static void push(node_t* u, node_t* v, edge_t* e, short dir, int flow, thread_t* thread)
{
	/* Assumes you hold all necessary locks */

	if (dir) {
		e->f += flow;
	} else {
		e->f -= flow;
	}

	u->e -= flow;
	v->e += flow;

	pr("@%d: pushing from %d to %d: fe = %d, eu = %d, ev = %d, c = %d, d = %d\n", 
		thread->thread_id, id(thread->g, u), id(thread->g, v), e->f, u->e, v->e, e->c, flow);
	count_stat(thread->stats.pushes);

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

		enter_excess(thread->g, v, thread);
	} 
	else if (v->e == 0) {
		// Must be the sink!
		pr("@%d: Sink completely filled\n", thread->thread_id);
		assert(v == thread->g->t);
		clear_flags(thread->threads);

		pthread_cond_broadcast(&thread->g->excess.cond);	// ERROR: maybe wonky
	}
}

static void relabel(graph_t* g, node_t* u, thread_t* thread)
{
	int u_h;

	// Todo: Faster with fetch_add? Okay with both relaxed?
	u_h = atomic_load_explicit(&u->h, memory_order_relaxed);
	atomic_store_explicit(&u->h, u_h + 1, memory_order_release);

	count_stat(thread->stats.relabels);
	pr("@%d: relabel %d now h = %d\n", thread->thread_id, id(g, u), u->h);

}


static void source_pushes(graph_t* g, thread_t* thread)
{
	adj_t* adj;
	node_t* s;
	node_t* v;
	edge_t* e;
	int n;
	int i;

	count_stat(thread->stats.nodes_processed);
	thread->nbr_nodes += NUM_THREADS*3;		// Ugly

	pr("@%d: Starting source pushes\n", thread->thread_id);

	s = g->s;
	n = s->h;

	pthread_mutex_lock(&s->mutex);
	
	adj = s->adj;

	// Try to push to all neighbours.
	for (i = 0; i < s->nbr_edge; i += 1) {

		e = &g->e[adj[i].index];
		v = &g->v[adj[i].v];

		// Hack to make s have "infinite preflow"
		s->e += e->c;	// TODO: Remove?

		/* helper func tp check if we can push and maybe do it */
		pr("@%d: Trying source push to %d == %d\n", thread->thread_id, id(g, v), adj[i].v);
		try_push(s, v, e, adj[i].dir, g, thread);

		
	}

	pthread_mutex_unlock(&s->mutex);

	thread->nbr_nodes -= NUM_THREADS*3;

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

static int can_push(node_t* u, node_t* v, edge_t* e, short dir)
{
	/* Returns how much u can push to v. Assumes you have observed if u has changed flow over e */

	int flow;

	assert(u->e > 0);

	if (dir) {
		flow = MIN(u->e, e->c - e->f);
		pr("pos: %d\n", flow);
	} else {
		flow = MIN(u->e, e->c + e->f);
		pr("neg: %d\n", flow);
	}

	// ERROR: Look out for memory order problems
	if (flow && atomic_load_explicit(&u->h, memory_order_relaxed) > 
				atomic_load_explicit(&v->h, memory_order_relaxed)){
		return flow;
	}
	else {
		return 0;
	}

}

static void try_push(node_t* u, node_t* v, edge_t* e, short dir, graph_t* g, thread_t* thread)
{
	/* Tries pushing from u to v, but only if possible.
	*/

	int flow;

	flow = can_push(u, v, e, dir);

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
		push(u, v, e, dir, flow, thread);

		pthread_mutex_unlock(&v->mutex);
	}
	else
	{
		pr("@%d: aborted push from %d to %d since flow = %d\n", thread->thread_id, id(g, u), id(g, v), flow);
	}
}

static void process_node(node_t* u, graph_t* g, thread_t* thread)
{
	/* Pushes and relables a u until it has no excess preflow.
	* 
	* Adds new nodes with excess preflow to some queue.
	*/

	adj_t* adj;
	edge_t* e;
	node_t *neigh, *v;
	int i;

	pthread_mutex_lock(&u->mutex);

	assert(u->e > 0);

	while (1) {

		adj = u->adj;
		// Try to push to all neighbours.
		while(adj - u->adj < u->nbr_edge && u->e > 0) {

			e = &g->e[adj->index];
			v = &g->v[adj->v];
			

			/* helper func tp check if we can push and maybe do it */
			try_push(u, v, e, adj->dir, g, thread);
			adj++;
			
		}

		if (u->e == 0){
			break;
		}

		relabel(g, u, thread);
	}

	pthread_mutex_unlock(&u->mutex);

}



static node_t* leave_excess(graph_t* g, thread_t* thread)
{
	node_t* u;

	if (!atomic_flag_test_and_set_explicit(&thread->cont, memory_order_relaxed))
	{
		return NULL;
	}
	else if (thread->next == NULL) 
	{
		u = leave_global_excess(g, thread);

		if (u != NULL) {pr("@%d: getting global node: %d\n", thread->thread_id, id(g, u));}
	}
	else 
	{
		u = leave_private_excess(thread);

		pr("@%d: getting private node: %d\n", thread->thread_id, id(g, u));
	}

	return u;
}

static void init_thread(thread_t* thread, init_info_t* init, pthread_barrier_t* barrier)
{
	*thread = (thread_t){	.thread_id = init->thread_id, 	//TODO: Remove from struct?
						.next = NULL, 
						.nbr_nodes = 0,
						.g = NULL,
						.cont = 1,	
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
	thread->nbr_nodes = 0;
	thread->next = NULL;
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
		g = thread.g;

		pr("@%d: Starting a new graph with n = %d, m = %d\n", thread.thread_id, g->n, g->m);

		if (thread.thread_id == 0) {
			source_pushes(g, &thread);
		}

		while((u = leave_excess(g, &thread)) != NULL)
		{
			pr("@%d: selected u = %d with h = %d and e = %d\n", thread.thread_id, id(g, u), u->h, u->e);

			process_node(u, g, &thread);
			count_stat(thread.stats.nodes_processed);
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

