#include <stdatomic.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>

#define COLLECT_STATS 0	/* enable/disable exit prints of stats as well as their collection */
#define FORSETE		1	/* remove main and change edge creation for Forsete */
#define PRINT		0	/* enable/disable prints. */
#define NUM_THREADS 20

#define NDEBUG
#include <assert.h>

#if COLLECT_STATS
#define count_stat(x) 	(x += 1);
#else
#define count_stat(x)	
#endif

#if PRINT

pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;

#define pr(...)		do {	pthread_mutex_lock(&print_lock);		\
							printf(__VA_ARGS__);		\
							pthread_mutex_unlock(&print_lock);	\
					} while (0)
#else
#define pr(...)
#endif

#define MIN(a,b)	(((a)<=(b))?(a):(b))


typedef struct graph_t	graph_t;
typedef struct node_t	node_t;
typedef struct edge_t	edge_t;
typedef struct elist_t	elist_t;
typedef struct thread_t thread_t;
typedef struct init_info_t init_info_t;

// TODO: toggle for this?
typedef struct xedge_t	xedge_t;
struct xedge_t {
	int32_t		u;	/* one of the two nodes.	*/
	int32_t		v;	/* the other. 			*/
	int32_t		c;	/* capacity.			*/
};

#if COLLECT_STATS
typedef struct thread_stats_t thread_stats_t;
struct thread_stats_t {
	unsigned int nbr_nodes;
	unsigned int pushes;
	unsigned int relabels;
	unsigned int iter;
	unsigned int missed_iter;
};
#endif

struct thread_t {
	const int thread_id;
	graph_t* g;
	node_t* active;	/* Private work queue, for the central one to access */
	int nbr_active;	/* Nbr of nodes in the private work queue, so main can easily rearrange */

#if COLLECT_STATS
	thread_stats_t stats;
#endif
};

struct init_info_t {
	graph_t* g;
	int thread_id; 
};

struct elist_t {
	edge_t*			edge;
	elist_t*		next;
};

struct node_t { // TODO: Only have one height and use it in transactions?
	int		h;	/* height.			*/
	int 	h_local;		/* height which can be overwritten in phase 1 */	
	int		e_diff;			/* shared incoming flow in phase 1 */
	int 	e_local;	/* e_copy minus the flow from pushes during the phase */
	char	activated;	/* has the node been activated by a push this phase? TODO: Make it so work can be done one these in some way. */
	char 	active;		/* Is the node active (pushing/relabeling) this phase? */
	elist_t*	edge;	/* adjacency list.		*/
	node_t*		next;	/* next node with instruction TODO: Maybe better to change to array in thread	*/
};

struct edge_t {
	node_t*		u;	/* one of the two nodes.	*/
	node_t*		v;	/* the other. 			*/
	int		f;	/* flow > 0 if from u to v.	Keep un-atomic. */
	int		c;	/* capacity.			*/
};

struct graph_t {
	int		n;	/* nodes.			*/
	int		m;	/* edges.			*/
	node_t*		v;	/* array of n nodes.		*/
	edge_t*		e;	/* array of m edges.		*/
	node_t*		s;	/* source.			*/
	node_t*		t;	/* sink.			*/

	thread_t** threads;		/* Array of pointers to the thread_t's */

	// Should these really be in here?
	pthread_barrier_t barrier;	/* Barrier for synchronizing all NUM_THREADS + 1 threads. */
	pthread_t pthreads[NUM_THREADS];		/* running threads */
	char done;
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
static void free_graph(graph_t* g);

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

static void add_edge(node_t* u, edge_t* e)
{
	elist_t*		p;

	

	/* allocate memory for a list link and put it first
	 * in the adjacency list of u.
	 *
	 */

	p = xmalloc(sizeof(elist_t));
	p->edge = e;
	p->next = u->edge;
	u->edge = p;
}

static void connect(node_t* u, node_t* v, int c, edge_t* e)
{
	/* connect two nodes by putting a shared (same object)
	 * in their adjacency lists.
	 *
	 */

	e->u = u;
	e->v = v;
	e->c = c;

	add_edge(u, e);
	add_edge(v, e);
}

#if FORSETE
static void init_edges_forsete(graph_t* g, xedge_t* e)
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
		connect(u, v, c, g->e+i);
	}
}

#else

static void init_edges_normal(graph_t* g)
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
		connect(u, v, c, g->e+i);
	}
	
	fclose(stdin);

}
#endif

static graph_t* new_graph(int n, int m, int s, int t, xedge_t* e)
{
	FILE* in;
	graph_t*	g;
	node_t*		u;
	node_t*		v;
	int		i;
	int		a;
	int		b;
	int		c;
	
	g = xmalloc(sizeof(graph_t));

	g->n = n;
	g->m = m;
	
	pr("n = %d\n", n);

	g->v = xcalloc(n, sizeof(node_t));
	g->e = xcalloc(m, sizeof(edge_t));

	g->s = &g->v[s];
	g->t = &g->v[t];
	g->s->h = n;

	g->threads = xmalloc(NUM_THREADS*sizeof(thread_t*));	/* TODO: change to no pointer? */

	pthread_barrier_init(&g->barrier, NULL, NUM_THREADS + 1);
	g->done = 0;

	// Should this be done with precompiler if? Seems like bad practice and compiler should optimize away?
#if (FORSETE)
	init_edges_forsete(g, e);
#else 
	init_edges_normal(g);
#endif

	return g;
}

static void push(node_t* u, node_t* v, edge_t* e, int flow, thread_t* attr)
{
	/** Pushes from u to v as described in run 
	*/
	
	int active;
	int activated;

	if (u == e->u) {
		e->f += flow;	//ONLY ONE CAN PUSH SO WE CAN UPDATE E FREELY!
	} else {
		e->f -= flow;
	}

	pr("@%d: pushing from %d to %d: f = %d, c = %d, added = %d\n", attr->thread_id, id(attr->g, u), id(attr->g, v), e->f, e->c, flow);

	u->e_local -= flow;

	pr("@%d: u->e_local = %d\n", attr->thread_id, u->e_local);

	active = v->active;
	activated = 0;

	// Update the e_diff of v and maybe activate it.
	__transaction_atomic {
		v->e_diff += flow;

		// TODO: maybe move active check and add out of here?
		if (!active && !v->activated) {
			v->activated = 1;
			activated = 1;
		}
	}

	if (activated && v != attr->g->t) {
		pr("@%d: %d activated and added to active\n", attr->thread_id, id(attr->g, v));

		v->next = attr->active;
		attr->active = v;

		attr->nbr_active += 1;
	}

	count_stat(attr->stats.pushes);

	/* the following are always true. */
	assert(flow > 0);
	assert(u->e_local >= 0 || u == attr->g->s);
	assert(abs(e->f) <= e->c);

}

static void relabel(node_t* u, thread_t* attr)
{
	/* Relabels the node by updating h_local, which can overwrite h later */

	u->h_local += 1;

	count_stat(attr->stats.relabels);

	pr("@%d: relabel %d now h = %d\n", attr->thread_id ,id(attr->g, u), u->h_local);

	assert(u->h_local < attr->g->n*2+1);

}

static node_t* other(node_t* u, edge_t* e)
{
	if (u == e->u)
		return e->v;
	else
		return e->u;
}

static void enter_excess(node_t* u, thread_t* thread, graph_t* g)
{
	// TODO: TEST FIFO?

	assert(u->active);
	assert(u != g->t);

	if (u != g->t && /* u != g->s && */ u->h < g->n){
		pr("MAIN: Entering %d into @%d\n", id(g, u), thread->thread_id);
		u->next = thread->active;
		thread->active = u;

		
	}
}

static void source_pushes(graph_t* g)
{
	elist_t* p;
	node_t *s, *v;
	edge_t* e;
	int thread_assign;

	thread_assign = 0;

	s = g->s;
	p = s->edge;

	// Try to push to all neighbours.
	while(p != NULL) {

		e = p->edge;
		p = p->next;
		v = other(s, e);

		// Push
		pr("@MAIN: Source pushing to %d\n", id(g, v));
		if (s == e->u) {
			e->f += e->c;
		} else {
			e->f -= e->c;
		}
		
		v->e_local = e->c;

		//Assign to thread
		if (v != g->t){

			v->h_local = 1;
			v->h = 1;
			v->active = 1;

			enter_excess(v, g->threads[thread_assign++ % NUM_THREADS], g);
		}
		
	}

	g->t->e_diff += g->t->e_local;

}

static void initialize_threads(init_info_t* init_info, graph_t* g)
{
	int i;
	for (i=0; i<NUM_THREADS; i++){
		init_info[i] = (init_info_t) {.g = g, .thread_id = i};
		pthread_create(&g->pthreads[i], NULL, &run, &init_info[i]);
	}
}

static void join_threads(graph_t* g)
{
	int i;
	for (i=0; i<NUM_THREADS; i++){
		pthread_join(g->pthreads[i], NULL);
	}
}

static void update_node(node_t* u, graph_t* g)
{
	if (u->active) {
		u->e_local += u->e_diff;
		u->e_diff = 0;

		u->h = u->h_local;

	} else {
		u->e_local = u->e_diff;	//TODO: can set this in thread, but will it make a difference?
		u->e_diff = 0;
	}

	pr("@MAIN: node %d now has e: %d, h: %d\n", id(g, u), u->e_local, u->h);

	assert(u->e_local >= 0);
}

static int do_changes(graph_t* g)
{
	/**
	 * Updates height and preflow of active nodes
	 */

	int thread_ind;
	int thread_assign;
	int found_work;
	int i;
	int e;
	thread_t* thread;
	node_t* u;
	node_t* next;
	node_t* active[NUM_THREADS];

	thread_assign = 0;
	found_work = 0;

	// TODO: ugly
	for (thread_ind = 0; thread_ind < NUM_THREADS; thread_ind += 1)
	{
		active[thread_ind] = g->threads[thread_ind]->active;
		g->threads[thread_ind]->active = NULL;
	}

	for (thread_ind = 0; thread_ind < NUM_THREADS; thread_ind += 1)
	{

		pr("@MAIN: processing thread: %d\n", thread_ind);
		// DO all changes for one thread
		thread = g->threads[thread_ind];

		u = active[thread_ind];

		while (u != NULL)
		{
			next = u->next;
			update_node(u, g);
			u->activated = 0;

			if (u->e_local > 0 && u->h < g->n) {
				u->active = 1;
				found_work = 1;		//Slow?
				enter_excess(u, g->threads[thread_assign++ % NUM_THREADS], g);
			} else {
				u->active = 0;
			}

			u = next;
		}
	}

	// TODO: Check if sink saturated.

	return found_work;
}

static void terminate_threads(graph_t* g)	// CHECK: Does this work well? cleanup?
{
	int i;

	for (i = 0; i < NUM_THREADS; i += 1)
	{
		pthread_cancel(g->pthreads[i]);
	}
}

static int xpreflow(graph_t* g)
{
	int i;
	int f;

	// Initialize all threads
	init_info_t* init_infos = xmalloc(NUM_THREADS*sizeof(init_info_t));
	initialize_threads(init_infos, g);
	
	pr("Waiting for threads to assign thread pointers in g\n");
	pthread_barrier_wait(&g->barrier);

	pr("Starting source pushes\n");
	// 
	source_pushes(g);

	do
	{
		pr("@MAIN: ending phase\n");

		pthread_barrier_wait(&g->barrier);	// Starts the other threads
		pthread_barrier_wait(&g->barrier);	// Start doing all the work

		pr("@MAIN: starting phase\n");		

	} while (do_changes(g));

	pr("@MAIN: work done\n");
	g->done = 1;

	pthread_barrier_wait(&g->barrier); // Release threads
	pr("@MAIN: released thread\n");

	f = g->t->e_diff;
	free(init_infos);

	pr("@MAIN: joining threads\n");
	join_threads(g);

	return f;

}

static int can_push(node_t* u, node_t* v, edge_t* e)
{
	int d;

	assert(u->e_local > 0);
	if (!(!v->active && u->h_local > v->h || u->h_local > v->h && u < v)) {
		d = 0;
	} else if (u == e->u) {
		d = MIN(u->e_local, e->c - e->f);
	} else {
		d = MIN(u->e_local, e->c + e->f);
	}

	return d;
}

static void process_node(node_t* u, thread_t* attr)
{
	/**
	 * Looks if there is any work to be done for the node.
	 */

	int flow;
	elist_t* list;
	edge_t* edge;
	node_t* v;

	assert(u->active != u->activated);
	assert(u->e_local > 0 || u->e_diff > 0);
	assert(u != attr->g->s);
	assert(u != attr->g->t);
	assert(u->h < attr->g->n);	// Only for efficiency

	count_stat(attr->stats.nbr_nodes);
	pr("@%d: processing: %d, h_local = %d, e_local = %d\n", attr->thread_id, id(attr->g, u), u->h_local, u->e_local);

	while (u->e_local > 0){

		list = u->edge;
		while(list != NULL && u->e_local > 0)	// TODO: if e_local = 0, update with e_diff?
		{
			edge = list->edge;
			list = list->next;
			v = other(u, edge);

			flow = can_push(u, v, edge);
			if (flow > 0)
			{
				push(u, v, edge, flow, attr);
			}
		}

		if (u->e_local > 0){
			relabel(u, attr);
		}
	}

	pr("@%d: finished processing: %d, h_local = %d, e_local = %d\n", attr->thread_id, id(attr->g, u), u->h_local, u->e_local);



}

static void calculate_changes(thread_t* attr)
{
	/**
	 * Go though the assigned active nodes,
	 * For each node try to do as many pushes and possibly one relabel.
	 * 
	 * A push is reflected in changing the e_local of the node and 
	 * not letting it get smaller than 0.
	 * 
	 * When wanting to relabel it can update h_local which then the main thread
	 * can overwrite h with in the second phase.
	 * 
	 * If it pushes to an inactive node it adds it to its active list but
	 * (TODO: ?) does not push from it this iteration. Instead just there
	 * to let the main thread find the active nodes easier.
	 * 
	 * If one node u tries to push to another active node v then u has to have
	 * either !v.active && u.h_local > v.h or u.h_local > v.h && u < v. To resolve conflicts.
	 *  
	 */
	int i;
	node_t* u;

	u = attr->active;
	while (u != NULL)
	{
		process_node(u, attr);

		assert(u != u->next);
		u = u->next;
	}
}

#if (COLLECT_STATS)
static void print_stats(void* arg)
{
	thread_t* attr = arg;

	printf("@%d: exiting, nodes: %d, pushes: %d, relabels: %d, iter: %d, missed iter: %d\n", 
		attr->thread_id, attr->stats.nbr_nodes, attr->stats.pushes, 
		attr->stats.relabels, attr->stats.iter, attr->stats.missed_iter);
	
}
#endif

static void* run(void* arg)
{
	/* Run method for the threads */

	init_info_t* init = arg;
	node_t* u;

	thread_t attr = {		.thread_id = init->thread_id, 
							.g = init->g,
							.active = NULL,

							#if COLLECT_STATS
							.stats = {0}
							#endif
							};

	attr.g->threads[attr.thread_id] = &attr;
	pr("@%d: Assigned thread pointer\n", attr.thread_id);
	pthread_barrier_wait(&attr.g->barrier);

	#if COLLECT_STATS
	// TODO: change to normal function like in lab 2
	pthread_cleanup_push(&print_stats, &attr);
	#endif

	pthread_barrier_wait(&attr.g->barrier);	// Start phase

	while(!attr.g->done)
	{
		
		pr("@%d: starting phase \n", attr.thread_id);

		calculate_changes(&attr);

		
		#if COLLECT_STATS
		if (attr.active == NULL) count_stat(attr.stats.missed_iter);
		#endif

		pr("@%d: ending phase\n", attr.thread_id);
		count_stat(attr.stats.iter);

		pthread_barrier_wait(&attr.g->barrier);	// End phase
		pthread_barrier_wait(&attr.g->barrier);	// Start phase

	}

	pr("exiting\n");

	#if COLLECT_STATS
	// I don't like having to have this here... But it seems to defined as macro needing both push and pop.
	pthread_cleanup_pop(1);
	#endif

	return NULL;
}

// TODO: Don't free after each algorithm
static void free_graph(graph_t* g)
{
	int		    i;	
	elist_t 		*p, *q;	

	pr("Freeing graph\n");

	for (i = 0; i < g->n; i += 1) {
		p = g->v[i].edge;
		while (p != NULL) {
			q = p->next;
			free(p);
			p = q;
		}
	}

	free(g->threads);

	free(g->v);
	free(g->e);
	free(g);

	pr("Graph free\n");

}

int preflow(int n, int m, int s, int t, xedge_t* e)
{
	graph_t* g;	//TODO: have on stack? Or just static?
	int f;

	g = new_graph(n, m, s, t, e);

	// Initialize threads to do the work and wait until they are done, then get the result.
	f = xpreflow(g);

	free_graph(g);

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

