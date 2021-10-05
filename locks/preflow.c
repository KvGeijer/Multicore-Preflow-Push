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

#include <assert.h>

#define FORSETE		1
#define NDEBUG



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

#define pr(...)		do {	pthread_mutex_lock(&print_lock);		\
							printf(__VA_ARGS__);		\
							pthread_mutex_unlock(&print_lock);	\
					} while (0)
#else
#define pr(...)
#endif

#define MIN(a,b)	(((a)<=(b))?(a):(b))


/* introduce names for some structs. a struct is like a class, except
 * it cannot be extended and has no member methods, and everything is
 * public.
 *
 * using typedef like this means we can avoid writing 'struct' in 
 * every declaration. no new type is introduded and only a shorter name.
 *
 */

typedef struct graph_t	graph_t;
typedef struct node_t	node_t;
typedef struct edge_t	edge_t;
typedef struct list_t	list_t;
typedef struct locked_node_list_t	locked_node_list_t;
typedef struct thread_t thread_t;
typedef struct init_info_t init_info_t;
typedef struct stats_t stats_t;

// TODO: toggle for this?
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
	int thread_id;
	node_t* next;	/* Private work queue */
	int nbr_nodes;

#if COLLECT_STATS
	stats_t stats;
#endif
};

struct init_info_t {
	graph_t* g;
	int thread_id; 
};

struct list_t {
	edge_t*		edge;
	list_t*		next;
};

struct locked_node_list_t {
	int waiting;
	int size;
	node_t* u;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
};

struct node_t {
	atomic_int	h;	/* height.			*/
	int		e;	/* excess flow.			*/
	list_t*		edge;	/* adjacency list.		*/
	node_t*		next;	/* with excess preflow.		*/
	pthread_mutex_t mutex; 	/* processing lock */
};

struct edge_t {
	node_t*		u;	/* one of the two nodes.	*/
	node_t*		v;	/* the other. 			*/
	int		f;	/* flow > 0 if from u to v.	*/
	int		c;	/* capacity.			*/
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

typedef struct static_graph_t static_graph_t;
struct static_graph_t {
	int n;
	int m;
	node_t* v;
	edge_t* e;
	list_t* links;
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
static void try_push(node_t* u, node_t* v, edge_t* e, graph_t* g, thread_t* attr);
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

	assert(0);

	p = xmalloc(n * s);

	/* memset sets everything (in this case) to 0. */
	memset(p, 0, n * s);

	return p;
}

static void print_stats(thread_t* attr)
{
#if (COLLECT_STATS)

	printf("@%d: exiting, nodes: %d, central pops: %d, pushes: %d, nonsaturated pushes: %d, relabels: %d\n", 
		attr->thread_id, attr->stats.nodes_processed, attr->stats.central_pops,
		attr->stats.pushes, attr->stats.nonsaturated_pushes, attr->stats.relabels);
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

	//TODO: Assign statically?

	pthread_mutex_init(&list->mutex, NULL);
	pthread_cond_init(&list->cond, NULL);

	list->u = NULL;
	list->size = 0;
	list->waiting = 0;
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

static void init_static_parts(graph_t* g, static_graph_t* stat_g)
{
	int i;

	stat_g->n = g->n;
	stat_g->m = g->m;

	stat_g->v = xcalloc(g->n, sizeof(node_t));
	stat_g->e = xcalloc(g->m, sizeof(edge_t));
	stat_g->links = xcalloc(g->m * 2, sizeof(list_t));

	g->v = stat_g->v;
	g->e = stat_g->e;

	for (i = 0; i < g->n; i++) {
		pthread_mutex_init(&g->v[i].mutex, NULL);
	}

}

static void update_static_parts(graph_t* g, static_graph_t* stat_g)
{
	int i;
	int j;

	// Nodes
	if (g->n > stat_g->n){
		stat_g->v = realloc(stat_g->v, g->n * sizeof(node_t));

		for (i = stat_g->n; i < g->n; i++) {
			pthread_mutex_init(&stat_g->v[i].mutex, NULL);
		}

	} 
	g->v = stat_g->v;
	
	// TODO: Move this to destroy_graph?
	for (i = 1; i < g->n; i += 1)
	{
		g->v[i].e = 0;
		atomic_store_explicit(&g->v[i].h, 0, memory_order_relaxed);
		g->v[i].next = NULL;
		g->v[i].edge = NULL;
	}
	g->v[0].e = 0;
	g->v[0].edge = NULL;


	// Edges
	if (g->m > stat_g->m){
		stat_g->e = realloc(stat_g->e, g->m * sizeof(edge_t));
		stat_g->links = realloc(stat_g->links, g->m * 2 * sizeof(edge_t));
	}
	g->e = stat_g->e;

	for (j = 0; j < g->m; j += 1)
	{
		g->e[j].f = 0;
	}

}

static void adjust_t_height(graph_t* g)
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
	atomic_store_explicit(&g->t->h, -max_cap, memory_order_relaxed);	//Just to save space, could alloc to g instead
	// ERROR: Check if this causes error, maybe not cleaned up right.

}

static void init_graph(graph_t* g, int n, int m, int s, int t, xedge_t* e)
{
	node_t*		u;
	node_t*		v;
	int		i;
	int		a;
	int		b;
	int		c;

	static static_graph_t* stat_g;

	g->n = n;
	g->m = m;

	if (stat_g == NULL) {
		stat_g = xcalloc(1, sizeof(static_graph_t));
		init_static_parts(g, stat_g);
	} else {
		update_static_parts(g, stat_g);
	}

	// TODO: Use OpenMP to divide into sections and parallelize loops

	g->s = &g->v[s];
	g->t = &g->v[t];
	atomic_store_explicit(&g->s->h, n, memory_order_release);

	init_lockedList(&g->excess);

#if (FORSETE)
	init_edges_forsete(g, e, stat_g);
#else 
	init_edges_normal(g, stat_g);
#endif

	adjust_t_height(g);

}

static void enter_private_excess(node_t* v, thread_t* attr)
{
	/* Enter the node v into the private work queue.
	 * 
	 * Atm just Lifo queue, but maybe Fifo better?
	 * 
	 * Really does not need you to hold the lock for v. But so fast...
	 */

	v->next = attr->next;
	attr->next = v;
	attr->nbr_nodes += 1;

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

static void enter_excess(graph_t* g, node_t* v, thread_t* attr)
{
	/* Put v into a work queue depending on circumstance.
	 *
	 * Atm you hold the lock for v, but do you have to?
	 * 
	 */

	assert(v->e > 0);

	// TODO: Better to not read height here? Maybe load in advance? Better to have == sink/source in node attr?
	// TODO: Remove v == g->t? As it now has so much negative preflow :D
	if (v == g->t || v == g->s || atomic_load_explicit(&v->h, memory_order_relaxed) >= g->n) {
		return ;
	}

	// TODO: Good way to decide which queue to add to. 
	if (attr->nbr_nodes < LOCAL_QUEUE)
	{
		pr("@%d: entering private excess, nbr private = %d, nbr global? = %d\n", 
			attr->thread_id, attr->nbr_nodes, g->excess.size);

		enter_private_excess(v, attr);
	}
	else 
	{
		
		pr("@%d: entering global excess, nbr private = %d, nbr global = %d\n", 
			attr->thread_id, attr->nbr_nodes, g->excess.size);

		enter_global_excess(g, v);
	}
	
}

static node_t* leave_private_excess(thread_t* attr)
{
	node_t* u;

	u = attr->next;
	attr->next = u->next;
	attr->nbr_nodes -= 1;

	return u;
}

static node_t* leave_global_excess(graph_t* g, thread_t* attr)
{
	node_t*		v;

	/* take any u from the set of nodes with excess preflow
	 * and for simplicity we always take the first.
	 * 
	 */

	pthread_mutex_lock(&g->excess.mutex);

	// ERROR: Reading g->t->e is not thread safe... Maybe replace with an atomic flag?
	while (g->excess.u == NULL && g->t->e < 0)
	{

		// TODO: prettify!
		g->excess.waiting += 1;
		assert(g->excess.waiting <= NUM_THREADS);
		pr("@%d: waiting, waiting = %d\n", attr->thread_id, g->excess.waiting);

		if (g->excess.waiting < NUM_THREADS)
		{
			// normal case
			// TODO: Try busy wait
			pthread_cond_wait(&g->excess.cond, &g->excess.mutex);
			g->excess.waiting -= 1;
		}
		else if (g->excess.waiting == NUM_THREADS || g->t->e == 0) 
		{	
			break;
		}
		
	}
	
	if (g->excess.u == NULL || g->t->e == 0) {	// TODO: ugly
		// TODO: recycle threads. Maybe return null instead?
		pthread_mutex_unlock(&g->excess.mutex);
		pthread_cond_signal(&g->excess.cond);
		print_stats(attr);
		pthread_exit(NULL);
	}

	v = g->excess.u;

	g->excess.u = v->next;
	g->excess.size -= 1;

	pthread_mutex_unlock(&g->excess.mutex);

	count_stat(attr->stats.central_pops);
	return v;
}

static void push(graph_t* g, node_t* u, node_t* v, edge_t* e, int flow, thread_t* attr)
{
	/* Assumes you hold all necessary locks */

	if (u == e->u) {
		e->f += flow;
	} else {
		e->f -= flow;
	}

	pr("@%d: pushing from %d to %d: f = %d, c = %d, d = %d\n", 
		attr->thread_id, id(g, u), id(g, v), e->f, e->c, flow);
	count_stat(attr->stats.pushes);

	u->e -= flow;
	v->e += flow;

	/* the following are always true. */

#if COLLECT_STATS
	if (abs(e->f) != e->c){
		count_stat(attr->stats.nonsaturated_pushes);
	}
#endif

	assert(flow > 0);
	assert(u->e >= 0 || u == g->s);
	assert(abs(e->f) <= e->c);

	if (v->e == flow) {

		/* since v has d excess now it had zero before and
		 * can now push.
		 */

		enter_excess(g, v, attr);
	}
}

static void relabel(graph_t* g, node_t* u, thread_t* attr)
{
	int u_h;

	// Todo: Faster with fetch_add? Okay with both relaxed?
	u_h = atomic_load_explicit(&u->h, memory_order_relaxed);
	atomic_store_explicit(&u->h, u_h + 1, memory_order_release);

	count_stat(attr->stats.relabels);
	pr("@%d: relabel %d now h = %d\n", attr->thread_id, id(g, u), u->h);

}

static node_t* other(node_t* u, edge_t* e)
{
	if (u == e->u)
		return e->v;
	else
		return e->u;
}

static void source_pushes(graph_t* g, thread_t* attr)
{
	list_t* p;
	node_t *s, *v;
	edge_t* e;
	int n;

	pr("@%d: Starting source pushes\n", attr->thread_id);

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
		pr("@%d: Trying source push to %d\n", attr->thread_id, id(g, v));
		try_push(s, v, e, g, attr);
		
	}

	pthread_mutex_unlock(&s->mutex);

}


static int xpreflow(graph_t* g)
{
	init_info_t init_infos[NUM_THREADS];
	pthread_t pthreads[NUM_THREADS];

	int i; 
	int f;

	for (i = 0; i < NUM_THREADS; i++) {
		init_infos[i].g = g;
		init_infos[i].thread_id = i;
		pthread_create(&pthreads[i], NULL, &run, &init_infos[i]);
	}

	for (i = 0; i < NUM_THREADS; i++){
		pthread_join(pthreads[i], NULL);
	}

	f = g->t->e - g->t->h;	// ERROR: reset up from negative preflow at start
	return f;

}

static int can_push(node_t* u, node_t* v, edge_t* e)
{
	/* Returns how much u can push to v. Assumes you have observed if u has changed flow over e */

	int flow;

	assert(u->e > 0);

	if (u == e->u) {
		flow = MIN(u->e, e->c - e->f);
	} else {
		flow = MIN(u->e, e->c + e->f);
	}

	// TODO: Pre-compute u_h and pass around? Should be faster
	// ERROR: Look out for memory order problems
	if (flow && atomic_load_explicit(&u->h, memory_order_relaxed) > 
				atomic_load_explicit(&v->h, memory_order_relaxed)){
		return flow;
	}
	else {
		return 0;
	}

}

static void try_push(node_t* u, node_t* v, edge_t* e, graph_t* g, thread_t* attr)
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
		push(g, u, v, e, flow, attr);

		pthread_mutex_unlock(&v->mutex);
	}
	else
	{
		pr("@%d: aborted push from %d to %d\n", attr->thread_id, id(g, u), id(g, v));
	}
}

static void process_node(node_t* u, graph_t* g, thread_t* attr)
{
	/* Pushes and relables a u until it has no excess preflow.
	* 
	* Adds new nodes with excess preflow to some queue.
	*/

	list_t* p;
	edge_t* e;
	node_t *neigh, *v;

	pthread_mutex_lock(&u->mutex);

	while (u->e > 0) {

		p = u->edge;
		// Try to push to all neighbours.
		while(p != NULL && u->e > 0) {

			e = p->edge;
			p = p->next;
			v = other(u, e);

			/* helper func tp check if we can push and maybe do it */
			try_push(u, v, e, g, attr);
			
		}

		if (u->e == 0){
			break;
		}

		relabel(g, u, attr);
	}

	pthread_mutex_unlock(&u->mutex);

}



static node_t* leave_excess(graph_t* g, thread_t* attr)
{
	// TODO: change from stack?

	node_t* u;
	if (attr->next == NULL) 
	{
		u = leave_global_excess(g, attr);

		pr("@%d: getting local node: %d\n", attr->thread_id, id(g, u));
	}
	else 
	{
		u = leave_private_excess(attr);

		pr("@%d: getting global node: %d\n", attr->thread_id, id(g, u));
	}

	return u;
}

static void* run(void* arg)
{
	/* Run method for the threads */

	init_info_t* init = arg;
	node_t* u;
	graph_t* g = init->g;
	
	thread_t attr = {		.thread_id = init->thread_id, 
							.next = NULL, 
							.nbr_nodes = 0,
#if COLLECT_STATS
							.stats = {0},
#endif
							};

	pr("@%d: starting run\n", attr.thread_id);

	if (attr.thread_id == 0) {
		count_stat(attr.stats.nodes_processed);
		attr.nbr_nodes += NUM_THREADS*3;		// TODO: Ugly
		source_pushes(g, &attr);
		attr.nbr_nodes -= NUM_THREADS*3;
	}

	while(1)
	{
		u = leave_excess(g, &attr);
		pr("@%d: selected u = %d with h = %d and e = %d\n", attr.thread_id, id(g, u), u->h, u->e);

		process_node(u, g, &attr);
		count_stat(attr.stats.nodes_processed);
	}

	return NULL;
}

static void destroy_graph(graph_t* g)
{
	int		i;
	list_t* p;
	list_t*	q;	

	// TODO: Do we need to do this? Statically allocate 2*m of them and re-use?
	/*for (i = 0; i < g->n; i += 1) {
		p = g->v[i].edge;
		while (p != NULL) {
			q = p->next;
			free(p);
			p = q;
		}
	}*/

	pthread_cond_destroy(&g->excess.cond);
	pthread_mutex_destroy(&g->excess.mutex);

}

int preflow(int n, int m, int s, int t, xedge_t* e)
{
	graph_t g;
	int f;

	init_graph(&g, n, m, s, t, e);

	f = xpreflow(&g);

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

