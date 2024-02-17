typedef struct node
{
	void *data;
	struct node *next;
} node_t;

typedef struct linkedList
{
	node_t *head;
	node_t *tail;
	int size;
} linkedList_t;

void linkedListInit(linkedList_t *ll);
void linkedList_destroy(linkedList_t *list);
void linkedListPush(linkedList_t *list, void *data);
void *linkedListPopHead(linkedList_t *ll);
void* linkedListPopTail(linkedList_t *ll);