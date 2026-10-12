#ifndef TRIE_H
#define TRIE_H

typedef struct trie{
    struct trie *children[256];
    int end;
}trie;

trie* createNode();
void insert(trie *root, char* str);
int search(trie *root, char* value);
int searchPrefix(trie *root, char* value);
void destroyTrie(trie *root);
int initializeData(trie *n, char *filename);
#endif