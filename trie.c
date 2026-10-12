
#include<stdio.h>
#include<stdlib.h>
#include<ctype.h>
#include<limits.h>
#include<string.h>
#include "trie.h"
#include "layerOne.h"

trie* createNode();
void insert(trie *root, char* str);
int search(trie *root, char* value);
int searchPrefix(trie *root, char* value);
void destroyTrie(trie *root);
int initializeData(trie*n, char *filename);

typedef struct trie{
    struct trie *children[256];
    int end;
}trie;

trie* createNode(){
    trie* n = malloc(sizeof(trie));
    for (int i = 0; i < 256; i++){
        n->children[i] = NULL;
    }
    n->end = 0;
    return n;
}

void insert(trie *root, char* str){
    char *i = str;
    trie *n = root;
    while(*i){
        unsigned char hashVal = (unsigned char) *i;
        if (n->children[hashVal] == NULL){
            n->children[hashVal] = createNode();
        }
        n = n->children[hashVal];
        i++;
    }
    n->end = 1;
}

int search(trie *root, char* value){
    char *i = value;
    trie *n = root;
    while (*i){
        unsigned char hashVal = (unsigned char) *i;;
        if (n->children[hashVal] == NULL){
            return 0;
        }
        n = n->children[hashVal];
        i++;
    }
    return n->end;
}

int searchPrefix(trie *root, char* value){
    char *i = value;
    trie *n = root;
    while (*i){
        unsigned char hashVal = (unsigned char) *i;;
        if (n->children[hashVal] == NULL){
            return 0;
        }
        n = n->children[hashVal];

        if (n->end == 1){
            break;
        }
        i++;
    }
    return n->end;
}

void destroyTrie(trie *root){
    if (root == NULL){
        return;
    }
    for (int i = 0,len = sizeof(root->children) / sizeof(root->children[0]); i < len; i++){
        destroyTrie(root->children[i]);
    }
    free(root);
}

void removeChar(char *str, char garbage) {
    char *src = str; // Pointer to read from
    char *dst = str; // Pointer to write to

    while (*src) {
        if (*src != garbage) {
            *dst = *src;
            dst++;
        }
        src++;
    }
    *dst = '\0'; // Properly terminate the shortened string
}

int initializeData(trie*n, char *filename){
    FILE *exclude = fopen(filename, "r");
    if (!exclude) {
        fprintf(stderr, "Warning: could not open file: '%s', continuing with no data\n", filename);
        return -1;                                                          
    }

    char name[PATH_MAX];
    int count = 0;
    while (1){
        if (fgets(name, sizeof(name), exclude) == NULL){
            break;
        }
        count++;
        name[strcspn(name, "\r\n")] = '\0';
        removeChar(name, '"');
        replaceAll(name, '\\', '/');
        insert(n, name);       
    }
    return count;
}
/*
int main(int argc, char argv[]){
    trie *n = createNode();
    initalizeData(n, "EXCLUDE.txt");
    int found = search(n, "ApdpData");
    printf("%d", found);

}
*/