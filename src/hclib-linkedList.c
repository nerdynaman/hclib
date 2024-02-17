#include "hclib-internal.h"

void linkedListInit(linkedList_t *ll)
{
	ll->head = NULL;
	ll->tail = NULL;
}

void linkedListPush(linkedList_t *ll, void *data)
{
	hc_mfence();
	node_t *node = (node_t *)malloc(sizeof(node_t));
	node->data = data;
	node->next = NULL;

	if (ll->head == NULL) {
		ll->head = node;
		ll->tail = node;
	} else {
		ll->tail->next = node;
		ll->tail = node;
	}
	ll->size++;
	hc_mfence();
}

void *linkedListPopHead(linkedList_t *ll)
{
	if (ll->head == NULL) {
		return NULL;
	}
	if (ll->head==ll->tail) {
		void *data = ll->head->data;
		free(ll->head);
		ll->head = NULL;
		ll->tail = NULL;
		ll->size--;
		return data;
	}

	node_t *node = ll->head;
	ll->head = ll->head->next;
	void *data = node->data;
	free(node);
	ll->size--;
	return data;
}

void* linkedListPopTail(linkedList_t *ll)
{
	if (ll->head == NULL) {
		return NULL;
	}
	if (ll->head==ll->tail) {
		void *data = ll->head->data;
		free(ll->head);
		ll->head = NULL;
		ll->tail = NULL;
		ll->size--;
		return data;
	}

	node_t *node = ll->head;
	node_t *prev = NULL;
	while (node->next != NULL) {
		prev = node;
		node = node->next;
	}
	if (prev == NULL) {
		ll->head = NULL;
		ll->tail = NULL;
	} else {
		prev->next = NULL;
		ll->tail = prev;
	}
	void *data = node->data;
	free(node);
	ll->size--;
	return data;
}