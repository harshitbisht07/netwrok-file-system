/* trie.h — byte-wide trie mapping file path -> storage-server id
 *
 * Rewritten: the old trie only had 26 children (a-z) and silently skipped every
 * other character, so "a.txt" and "atxt" were the same key, and removing the last
 * word freed the root node. This version:
 *   - handles every byte (case-sensitive, dots, digits, '/', spaces ...)
 *   - stays compact (first-child / next-sibling lists, not 256 pointers per node)
 *   - never frees the root
 *   - can purge every entry that belongs to one storage server (used when a
 *     storage server re-registers, so its index is rebuilt from scratch)
 * ssid values are always > 0; 0 means "not found".
 */
#ifndef TRIE_H
#define TRIE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TrieNode
{
    struct TrieNode *child;
    struct TrieNode *sibling;
    unsigned char    ch;
    int              isEndOfWord;
    int              ssid;
} TrieNode;

static inline TrieNode *createNode(void) { return (TrieNode *)calloc(1, sizeof(TrieNode)); }

static inline TrieNode *trie_find_child(TrieNode *n, unsigned char c)
{
    for (TrieNode *x = n->child; x; x = x->sibling)
        if (x->ch == c) return x;
    return NULL;
}

static inline void insertTrie(TrieNode *root, const char *word, int ssid)
{
    TrieNode *cur = root;
    for (const unsigned char *p = (const unsigned char *)word; *p; p++)
    {
        TrieNode *nx = trie_find_child(cur, *p);
        if (!nx)
        {
            nx = createNode();
            if (!nx) return;
            nx->ch = *p;
            nx->sibling = cur->child;
            cur->child = nx;
        }
        cur = nx;
    }
    cur->isEndOfWord = 1;
    cur->ssid = ssid;
}

/* Returns the ssid (>0) when the exact word is stored, otherwise 0. */
static inline int searchTrie(TrieNode *root, const char *word)
{
    TrieNode *cur = root;
    for (const unsigned char *p = (const unsigned char *)word; *p; p++)
    {
        cur = trie_find_child(cur, *p);
        if (!cur) return 0;
    }
    return cur->isEndOfWord ? cur->ssid : 0;
}

/* returns 1 when n is now empty and the caller should unlink + free it */
static inline int trie_remove_rec(TrieNode *n, const unsigned char *w)
{
    if (*w == '\0')
    {
        n->isEndOfWord = 0;
        n->ssid = 0;
        return n->child == NULL;
    }
    TrieNode **pp = &n->child;
    while (*pp && (*pp)->ch != *w) pp = &(*pp)->sibling;
    if (!*pp) return 0;
    if (trie_remove_rec(*pp, w + 1))
    {
        TrieNode *dead = *pp;
        *pp = dead->sibling;
        free(dead);
    }
    return n->child == NULL && !n->isEndOfWord;
}

static inline void removeTrie(TrieNode *root, const char *word)
{
    trie_remove_rec(root, (const unsigned char *)word);   /* root is never freed */
}

static inline int trie_purge_rec(TrieNode *n, int ssid)
{
    if (n->isEndOfWord && n->ssid == ssid) { n->isEndOfWord = 0; n->ssid = 0; }
    TrieNode **pp = &n->child;
    while (*pp)
    {
        TrieNode *c = *pp;
        if (trie_purge_rec(c, ssid)) { *pp = c->sibling; free(c); }
        else pp = &c->sibling;
    }
    return n->child == NULL && !n->isEndOfWord;
}

static inline void purgeTrieSS(TrieNode *root, int ssid) { trie_purge_rec(root, ssid); }

static inline void freeTrie(TrieNode *root)
{
    if (!root) return;
    freeTrie(root->child);
    freeTrie(root->sibling);
    free(root);
}

#endif /* TRIE_H */
