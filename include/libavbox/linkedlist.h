/**
 * avbox - Toolkit for Embedded Multimedia Applications
 * Copyright (C) 2016-2017 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3 as 
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#ifndef __LINKEDLIST_H__
#define __LINKEDLIST_H__

#define LIST	struct __listhead

struct __listhead
{
	struct __listhead* prev;
	struct __listhead* next;
};

#define LIST_DECLARE(var) \
	struct __listhead var

#define LISTABLE_TYPE(type, fields) \
typedef struct __ ## type \
{ \
	LIST_DECLARE(__ ## type ## _listhead); \
	fields \
} \
type


#define LISTABLE_STRUCT(type, fields) \
struct type \
{ \
	LIST_DECLARE(__ ## type ## _listhead); \
	fields \
}


#define LIST_DECLARE_STATIC(var) \
	static struct __listhead var

#define LIST_INIT(list) \
	((struct __listhead*)(list))->next = list; \
	((struct __listhead*)(list))->prev = list

#define LIST_EMPTY(list) (((struct __listhead*)(list))->next == (list))

#define LIST_INSERT(iitem, iprev, inext) \
{ \
	struct __listhead * const __prev = (struct __listhead*) iprev; \
	struct __listhead * const __next = (struct __listhead*) inext; \
	__next->prev = (struct __listhead*)(iitem); \
	((struct __listhead*)(iitem))->next = (struct __listhead*)(__next); \
	((struct __listhead*)(iitem))->prev = (struct __listhead*)(__prev); \
	__prev->next = (struct __listhead*)(iitem); \
}

#define LIST_ADD(list, item) \
	LIST_INSERT(item, list, (list)->next)

#define LIST_APPEND(list, item) \
	LIST_INSERT(item, (list)->prev, list)

#define LIST_REMOVE(item) \
{ \
        struct __listhead * const prev = ((struct __listhead*)(item))->prev; \
        struct __listhead * const next = ((struct __listhead*)(item))->next; \
        next->prev = prev; \
        prev->next = next; \
}

#define LIST_SWAP(a, b) \
{ \
	struct __listhead * const prev = ((struct __listhead*)(a))->prev; \
	struct __listhead * const next = ((struct __listhead*)(a))->next; \
	((struct __listhead*)(a))->prev = ((struct __listhead*)(b))->prev; \
	((struct __listhead*)(a))->next = ((struct __listhead*)(b))->next; \
	((struct __listhead*)(b))->prev = prev; \
	((struct __listhead*)(b))->next = next; \
}

#define LIST_ISNULL(list, item) (((struct __listhead*)(item)) == (list))
#define LIST_TAIL(type, list) ((type) (LIST_EMPTY((list)) ? NULL : ((struct __listhead*)(list))->prev))
#define LIST_NEXT(type, item) ((type) ((struct __listhead*)(item))->next)
#define LIST_PREV(type, item) ((type) ((struct __listhead*)(item))->prev)


#define LIST_FOREACH(type, ivar, list) \
	for (ivar = LIST_NEXT(type, list); ((struct __listhead*) ivar) != list; ivar = LIST_NEXT(type, ivar))

#define LIST_FOREACH_SAFE(type, var, list, codeblock) \
{ \
	struct __listhead* __next; \
	for (var = LIST_NEXT(type, list); ((struct __listhead*) var) != list; var = (type) __next) { \
		__next = (struct __listhead*) LIST_NEXT(type, var); \
		codeblock \
	} \
}

#define LIST_COUNT(list, sz) \
{ \
	struct __listhead *ent; \
	sz = 0; \
	LIST_FOREACH(struct __listhead*, ent, list) { \
		sz++; \
	} \
}


static inline size_t
LIST_SIZE(struct __listhead* list)
{
	size_t sz = 0;
	LIST_COUNT(list, sz);
	return sz;
}

#endif

