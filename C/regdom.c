/*
 * Calculate the effective registered domain of a fully qualified domain name.
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Florian Sager, 03.01.2009, sager@agitos.de, http://www.agitos.de
 * Ward van Wanrooij, 04.04.2010, ward@ward.nu
 * Ed Walker, 03.10.2012
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "regdom.h"

/* data types */

struct tldnode
{
    char *dom;
    const char *attr;
    unsigned int num_children;
    struct tldnode **subnodes;
};
typedef struct tldnode tldnode;

/* static data */

#include "tld-canon.h"

static const char ALL[] = "*";
static const char THIS[] = "!";

// helper function to parse node in tldString
static int
readTldString(tldnode *node, const char *s, int len, int pos)
{
    int start = pos;
    int state = 0;

    memset(node, 0, sizeof(tldnode));
    do
    {
        char c = s[pos];

        switch (state)
        {
        case 0: // general read
            if (c == ',' || c == ')' || c == '(')
            {
                // add last domain
                int lenc = node->attr == THIS ? pos - start - 1 : pos - start;
                node->dom = malloc(lenc + 1);
                memcpy(node->dom, s + start, lenc);
                node->dom[lenc] = 0;

                if (c == '(')
                {
                    // read number of children
                    start = pos;
                    state = 1;
                }
                else if (c == ')' || c == ',')
                    // return to parent domains
                    return pos;
            }
            else if (c == '!')
                node->attr = THIS;
            break;

        case 1: // reading number of elements (<number>:
            if (c == ':')
            {
                char *buf = malloc((pos - start - 1) + 1);
                memcpy(buf, s + start + 1, pos - start - 1);
                buf[pos - start - 1] = 0;
                node->num_children = atoi(buf);
                free(buf);

                // allocate space for children
                node->subnodes =
                    malloc(node->num_children * sizeof(tldnode *));

                for (unsigned int i = 0; i < node->num_children; i++)
                {
                    node->subnodes[i] = malloc(sizeof(tldnode));
                    pos = readTldString(node->subnodes[i], s, len, pos + 1);
                }

                return pos + 1;
            }
            break;
        }
        pos++;
    }
    while (pos < len);

    return pos;
}

// Read TLD string into fast-lookup data structure
void *
loadTldTree(void)
{
    tldnode *root = malloc(sizeof(tldnode));

    readTldString(root, tldString, sizeof tldString - 1, 0);

    return root;
}

static void
printTldTreeI(tldnode *node, const char *spacer)
{
    if (node->num_children != 0)
    {
        // has children
        printf("%s%s:\n", spacer, node->dom);

        for (unsigned int i = 0; i < node->num_children; i++)
        {
            char dest[100];
            sprintf(dest, "  %s", spacer);

            printTldTreeI(node->subnodes[i], dest);
        }
    }
    else
    {
        // no children
        printf("%s%s: %s\n", spacer, node->dom, node->attr);
    }
}

void
printTldTree(void *node, const char *spacer)
{
    printTldTreeI((tldnode *) node, spacer);
}

static void
freeTldTreeI(tldnode *node)
{
    for (unsigned int i = 0; i < node->num_children; i++)
        freeTldTreeI(node->subnodes[i]);
    free(node->subnodes);
    free(node->dom);
    free(node);
}

void
freeTldTree(void *root)
{
    freeTldTreeI((tldnode *) root);
}

// linear search for domain (and * if available)
static tldnode *
findTldNode(tldnode *parent, const char *seg_start, const char *seg_end)
{
    tldnode *allNode = 0;

    for (unsigned int i = 0; i < parent->num_children; i++)
    {
        if (!allNode && !strcmp(parent->subnodes[i]->dom, ALL))
            allNode = parent->subnodes[i];
        else
        {
            size_t m = seg_end - seg_start;
            size_t n = strlen(parent->subnodes[i]->dom);
            if (m == n && !memcmp(parent->subnodes[i]->dom, seg_start, n))
                return parent->subnodes[i];
        }
    }
    return allNode;
}

static char *
getRegisteredDomainDropI(const char *hostname, tldnode *tree,
                         int drop_unknown)
{
    // Eliminate some special (always-fail) cases first.
    if (hostname[0] == '.' || hostname[0] == '\0')
        return 0;

    // The registered domain will always be a suffix of the input hostname.
    // Start at the end of the name and work backward.
    const char *head = hostname;
    const char *seg_end = hostname + strlen(hostname);
    const char *seg_start;

    if (seg_end[-1] == '.')
        seg_end--;
    seg_start = seg_end;

    for (;;) {
        while (seg_start > head && *seg_start != '.')
            seg_start--;
        if (*seg_start == '.')
            seg_start++;

        // [seg_start, seg_end) is one label.
        tldnode *subtree = findTldNode(tree, seg_start, seg_end);
        if (!subtree
            || (subtree->num_children == 1
                && subtree->subnodes[0]->attr == THIS))
            // Match found.
            break;

        if (seg_start == head)
            // No match, i.e. the input name is too short to be a
            // registered domain.
            return 0;

        // Advance to the next label.
        tree = subtree;

        if (seg_start[-1] != '.')
            abort();
        seg_end = seg_start - 1;
        seg_start = seg_end - 1;
    }

    // Ensure the stripped domain contains at least two labels.
    if (!strchr(seg_start, '.'))
    {
        if (seg_start == head || drop_unknown)
            return 0;

        seg_start -= 2;
        while (seg_start > head && *seg_start != '.')
            seg_start--;
        if (*seg_start == '.')
            seg_start++;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
    return (char *)seg_start;
#pragma GCC diagnostic pop
}

char *
getRegisteredDomainDrop(const char *hostname, void *tree, int drop_unknown)
{
    return getRegisteredDomainDropI(hostname, (tldnode *) tree, drop_unknown);
}

char *
getRegisteredDomain(const char *hostname, void *tree)
{
    return getRegisteredDomainDropI(hostname, (tldnode *) tree, 0);
}
