#ifndef PARSER_C
#define PARSER_C

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "charset.h"
#include "metaparser.h"
#include "parser.h"
#include "set.h"
#include "ustring.h"

/**
 * Global data structures used by the parser
 * Slots are a head ::= rule, with a dot starting the rule, or following a non-terminals)
 */
vect* parser_symbol_firsts;         // vect containing the first set of each symbol.
vect* parser_symbol_follows;        // vect containing the follow set of each symbol.
dict* parser_substring_firsts_dict; // dict<slice, fset> which memoizes the first set of a substring
vect* parser_labels;

/**
 * Allocate global data structures used by the parser
 */
void allocate_parser()
{
    parser_symbol_firsts = new_vect();
    parser_symbol_follows = new_vect();
    parser_substring_firsts_dict = new_dict();
    parser_labels = new_vect();
}

/**
 * Initialize any global data structures used by the parser.
 *
 * Note: this function should only be run after the complete_metaparser()
 * has been called on a successful metaparse of a grammar.
 */
void initialize_parser(/*uint64_t source_length*/)
{
    parser_compute_symbol_firsts();
    parser_compute_symbol_follows();
}

/**
 * Free any global data structures used by the parser.
 */
void release_parser()
{
    vect_free(parser_symbol_firsts);
    vect_free(parser_symbol_follows);
    dict_free(parser_substring_firsts_dict);
    vect_free(parser_labels);
}

/**
 * Create a new parser context
 */
inline parser_context parser_context_struct(uint32_t* src, uint64_t len, uint64_t start_idx, bool whole, bool sub)
{
    parser_context con = (parser_context){
        .I = src,
        .m = len,
        .cI = 0,
        .cU = 0,
        .CRF = new_crf(),
        .P = new_dict(),
        .Y = new_dict(),
        .R = new_vect(),
        .U = new_set(),
        .whole = whole,
        .start_idx = start_idx,
        .sub = sub,
        .success = false,
    };
    return con;
}

/**
 * Free the allocated structures in a parser context
 */
void release_parser_context(parser_context* con)
{
    crf_free(con->CRF);
    dict_free(con->P);
    dict_free(con->Y);
    set_free(con->U);

    // make sure R is empty before freeing it
    while (vect_size(con->R) > 0) { vect_pop(con->R); }
    vect_free(con->R);
}

/**
 * Parse a given source string with the given context.
 */
bool parser_parse(parser_context* con)
{
    crf_cluster_node u0 = crf_cluster_node_struct(con->start_idx, 0);
    uint64_t node_idx = crf_add_cluster_node(con->CRF, &u0);
    parser_nonterminal_add(con->start_idx, 0, con);

    // for sub-parses, stop on first sign of success
    while (vect_size(con->R) > 0 && !(con->sub && con->success))
    {
        // remove a descriptor (L,k,j) from R. Descriptors are owned by U, so no need to free in here.
        desc* d = vect_dequeue(con->R)->data; // breadth first parse
        // desc* d = vect_pop(con->R)->data; // (alternative) depth first parse. performs worse.
        con->cU = d->k;
        con->cI = d->j;
        parser_handle_label(&d->L, con);
    }

    if (!con->sub)
    {
        // apply precedence filters to the BSR forest
        parser_apply_precedence_filters(con);
    }

    return con->success;
}

/**
 * Generate the list of labels (slots) used by the CNP algorithm for the current grammar
 */
void parser_generate_labels()
{
    // iterate over the list of productions in the metaparser
    dict* productions = metaparser_get_productions();
    for (size_t i = 0; i < dict_size(productions); i++)
    {
        obj head_idx_obj;
        obj bodies_set_obj;
        dict_get_at_index(productions, i, &head_idx_obj, &bodies_set_obj);
        uint64_t head_idx = *(uint64_t*)head_idx_obj.data;
        set* bodies = (set*)bodies_set_obj.data;

        for (size_t body_idx = 0; body_idx < set_size(bodies); body_idx++)
        {
            vect* body = metaparser_get_production_body(head_idx, body_idx);

            slot s = slot_struct(head_idx, body_idx, 0);

            // add the initial item for the production to the list of labels
            vect_append(parser_labels, new_slot_obj(slot_copy(&s)));

            // iterate over the slot until the dot is at the end of the production
            for (s.dot = 1; s.dot <= vect_size(body); s.dot++)
            {
                uint64_t* symbol_idx = vect_get(body, s.dot - 1)->data;
                // if the symbol before the dot is a non-terminal, add a new slot
                if (!metaparser_is_symbol_terminal(*symbol_idx))
                {
                    vect_append(parser_labels, new_slot_obj(slot_copy(&s)));
                }
            }
        }
    }
}

/**
 * return the list of labels generated by the parser for the current grammar
 */
vect* parser_get_labels() { return parser_labels; }

/**
 * perform the CNP parsing actions for the given label
 */
void parser_handle_label(slot* label, parser_context* con)
{
    // keep track of the current dot in the item without modifying the original
    uint64_t dot = label->dot;

    vect* body = metaparser_get_production_body(label->head_idx, label->production_idx);
    if (label->dot == 0 && vect_size(body) == 0)
    {
        bsr_head empty = new_prod_bsr_head_struct(label->head_idx, label->production_idx, con->cI, con->cI);
        parser_bsr_add_helper(&empty, con->cI, con);
    }
    else
    {
        // handle all sequential terminals in the production body
        while (dot < vect_size(body))
        {
            if (!metaparser_is_symbol_terminal(*(uint64_t*)vect_get(body, dot)->data)) { break; }
            if (dot != 0)
            {
                slice s = slice_struct(body, dot, vect_size(body));
                if (!parser_test_select(con->I[con->cI], label->head_idx, &s)) { return; }
            }
            dot++;

            parser_bsr_add(&(slot){label->head_idx, label->production_idx, dot}, con->cU, con->cI, con->cI + 1, con);
            con->cI++;
        }

        if (dot < vect_size(body))
        {
            if (dot != 0)
            {
                slice s = slice_struct(body, dot, vect_size(body));
                if (!parser_test_select(con->I[con->cI], label->head_idx, &s)) { return; }
            }
            dot++;
            parser_call(&(slot){label->head_idx, label->production_idx, dot}, con->cU, con->cI, con);
        }
    }

    // handle next non-terminal in the production body
    if (label->dot == vect_size(body) ||
        (dot == vect_size(body) && metaparser_is_symbol_terminal(*(uint64_t*)vect_get(body, dot - 1)->data)))
    {
        // check that the next input character is in the followset of this non-terminal
        fset* follow = parser_follow_of_symbol(label->head_idx);
        if (!fset_contains_c(follow, con->I[con->cI])) { return; }

        // check that this rule has no filters, or does not fail any of its filters
        if (!parser_rule_passes_filters(label->head_idx, con)) { return; }

        parser_return(label->head_idx, con->cU, con->cI, con);
    }
}

/**
 * print the CNP actions performed for the given label
 */
void parser_print_label(slot* label)
{
    slot_str(label);
    printf("\n");

    // keep track of the current dot in the item without modifying the original
    uint64_t dot = label->dot;

    vect* body = metaparser_get_production_body(label->head_idx, label->production_idx);
    if (label->dot == 0 && vect_size(body) == 0)
    {
        printf("    insert (");
        obj_str(metaparser_get_symbol(label->head_idx));
        printf(" -> ϵ, cI, cI, cI) into Y\n");
    }
    else
    {
        while (dot < vect_size(body))
        {
            if (!metaparser_is_symbol_terminal(*(uint64_t*)vect_get(body, dot)->data)) { break; }
            if (dot != 0)
            {
                slice s = slice_struct(body, dot, vect_size(body));
                printf("    if (!parser_test_select(I[cI], ");
                obj_str(metaparser_get_symbol(label->head_idx));
                printf(", ");
                parser_print_body_slice(&s);
                printf("))\n        goto L0\n");
            }
            dot++;
            printf("    parser_bsr_add(");
            slot_str(&(slot){label->head_idx, label->production_idx, dot});
            printf(", cU, cI, cI + 1);\n    cI += 1\n");
        }

        if (dot < vect_size(body))
        {
            if (dot != 0)
            {
                slice s = slice_struct(body, dot, vect_size(body));
                printf("    if (!parser_test_select(I[cI], ");
                obj_str(metaparser_get_symbol(label->head_idx));
                printf(", ");
                parser_print_body_slice(&s);
                printf("))\n        goto L0\n");
            }
            dot++;
            printf("    parser_call(");
            slot_str(&(slot){label->head_idx, label->production_idx, dot});
            printf(", cU, cI);\n");
        }
    }

    if (label->dot == vect_size(body) ||
        (dot == vect_size(body) && metaparser_is_symbol_terminal(*(uint64_t*)vect_get(body, dot - 1)->data)))
    {
        printf("    if (I[cI] ∈ follow(");
        obj_str(metaparser_get_symbol(label->head_idx));
        printf("))\n        rtn(");
        obj_str(metaparser_get_symbol(label->head_idx));
        printf(", cU, cI);\n");
    }
    printf("    goto L0\n");
}

/**
 * When about to return a completed return action, check to ensure that the result should not be excluded by either
 * nofollow or reject filters.
 */
bool parser_rule_passes_filters(uint64_t head_idx, parser_context* con)
{
    // check if this rule has any nofollow filters
    obj* filter = metaparser_get_nofollow_entry(head_idx);
    if (filter != NULL)
    {
        if (filter->type == CharSet_t)
        {
            if (con->I[con->cI] != 0 && charset_contains_c(filter->data, con->I[con->cI])) return false;
        }
        else if (filter->type == UnicodeString_t)
        {
            if (ustring_prefix_match(&con->I[con->cI], filter->data)) return false;
        }
        else
        {
            // perform a full parse starting at the end of the current location in the input
            if (filter->type != UInteger_t)
            {
                printf("ERROR: unkown type %d for nofollow filter. ", filter->type);
                obj_str(filter);
                printf("\n");
                exit(1);
            }
            // set up a new parsing context starting from cI to match for this rule
            uint64_t* head_idx = filter->data;
            parser_context subcon = parser_context_struct(&con->I[con->cI], con->m - con->cI, *head_idx, false, true);
            bool result = parser_parse(&subcon);
            release_parser_context(&subcon);
            if (result) return false; // success means the rule is rejected
        }
    }

    // check if this rule has any reject filters
    filter = metaparser_get_reject_entry(head_idx);
    if (filter != NULL)
    {
        if (filter->type == CharSet_t)
        {
            // note: this is unlikely as charset rejects are generally collapsed into single charsets
            if (con->cI - con->cU == 1 && charset_contains_c(filter->data, con->I[con->cU])) return false;
        }
        else if (filter->type == UnicodeString_t)
        {
            if (con->cI - con->cU == ustring_len(filter->data) && ustring_prefix_match(&con->I[con->cU], filter->data))
                return false;
        }
        else
        {
            // perform a full parse over the range of this rule in the input
            if (filter->type != UInteger_t)
            {
                printf("ERROR: unkown type %d for reject filter. ", filter->type);
                obj_str(filter);
                printf("\n");
                exit(1);
            }
            // set up a new parsing context starting from cU to match for this rule.
            uint64_t* head_idx = filter->data;
            uint32_t saved_char = con->I[con->cI]; // save the I[cI] so we can set it to \0 for the subparse
            con->I[con->cI] = 0;
            parser_context subcon = parser_context_struct(&con->I[con->cU], con->cI - con->cU, *head_idx, true, true);
            bool result = parser_parse(&subcon);
            release_parser_context(&subcon);
            con->I[con->cI] = saved_char; // restore the I[cI]
            if (result) return false;     // success means the rule is rejected
        }
    }

    // no filters disqualified this rule
    return true;
}

/**
 * Apply precedence filters to rules in the parse tree
 */
void parser_apply_precedence_filters(parser_context* con)
{
    // start with the root node
    // TODO. see dewy_compiler_compiler for how to get the root node(s)
}

/**
 * Add the nonterminal's productions to the list of pending process actions.
 */
void parser_nonterminal_add(uint64_t head_idx, uint64_t j, parser_context* con)
{
    set* bodies = metaparser_get_production_bodies(head_idx);
    for (size_t body_idx = 0; body_idx < set_size(bodies); body_idx++)
    {
        vect* body = metaparser_get_production_body(head_idx, body_idx);
        slice s = slice_struct(body, 0, vect_size(body));
        if (parser_test_select(con->I[j], head_idx, &s))
        {
            parser_descriptor_add(&(slot){head_idx, body_idx, 0}, j, j, con);
        }
    }
}

/**
 * Test if the current character can start the remaining production body, or follows the current production head.
 */
bool parser_test_select(uint32_t c, uint64_t head_idx, slice* string)
{
    fset* first = parser_memo_first_of_string(string);
    if (fset_contains_c(first, c)) { return true; }
    else if (first->special)
    {
        fset* follow = parser_follow_of_symbol(head_idx);
        if (fset_contains_c(follow, c)) { return true; }
    }
    return false;
}

/**
 * Add a new process descriptor to the list of pending descriptors, if it is not already there.
 * creates a copy of the slot if it is to be inserted. Original is not modified.
 */
void parser_descriptor_add(slot* L, uint64_t k, uint64_t j, parser_context* con)
{
    desc d = desc_struct(L, k, j);
    obj D = obj_struct(Descriptor_t, &d);
    if (!set_contains(con->U, &D))
    {
        desc* new_d = new_desc(L, k, j);
        obj* new_D = new_desc_obj(new_d);
        set_add(con->U, new_D);
        vect_enqueue(con->R, new_D); // new_D is owned by U, so when dequeued, it does not need to be freed
    }
}

/**
 * Complete the process of parsing the given symbol.
 */
void parser_return(uint64_t head_idx, uint64_t k, uint64_t j, parser_context* con)
{
    // check if P already contains the action to be returned
    crf_action_head a = crf_action_head_struct(head_idx, k);
    if (!crf_action_in_P(con->P, &a, j))
    {
        // add the action to P
        crf_add_action_to_P(con->P, &a, j);

        // get the children of the crf_cluster_node (head_idx, k)
        crf_cluster_node node = crf_cluster_node_struct(head_idx, k);
        obj* children_set_obj = dict_get(con->CRF->cluster_nodes, &(obj){CRFClusterNode_t, &node});
        if (children_set_obj != NULL)
        {
            set* children_set = children_set_obj->data;
            for (size_t i = 0; i < set_size(children_set); i++)
            {
                uint64_t* child_idx = set_get_at_index(children_set, i)->data;
                crf_label_node* child = set_get_at_index(con->CRF->label_nodes, *child_idx)->data;
                parser_descriptor_add(&child->label, child->j, j, con);
                parser_bsr_add(&child->label, child->j, k, j, con);
            }
        }
    }
}

/**
 * Initiate parsing actions for the given slot.
 */
void parser_call(slot* L, uint64_t i, uint64_t j, parser_context* con)
{
    // check to see if the dot is after a nonterminal. otherwise L is not of the form Y ::= αX · β
    if (L->dot == 0) { return; }
    vect* body = metaparser_get_production_body(L->head_idx, L->production_idx);
    uint64_t* X_idx = vect_get(body, L->dot - 1)->data;
    if (metaparser_is_symbol_terminal(*X_idx)) { return; }

    // check to see if the CRF node labelled (L, i) exists. If not, create it.
    crf_label_node u = crf_label_node_struct(L, i);
    uint64_t u_idx = crf_add_label_node(con->CRF, &u); // crf_add_cluster_node(con->CRF, &u);

    // check to see if the CRF node labelled (X, j) exists
    crf_cluster_node v = crf_cluster_node_struct(*X_idx, j);
    uint64_t v_idx = dict_get_entries_index(con->CRF->cluster_nodes, &(obj){CRFClusterNode_t, &v});
    obj v_obj, children_obj;
    if (!dict_get_at_index(con->CRF->cluster_nodes, v_idx, &v_obj, &children_obj))
    {
        v_idx = crf_add_cluster_node(con->CRF, &v);
        crf_add_edge(con->CRF, v_idx, u_idx);
        parser_nonterminal_add(*X_idx, j, con);
    }
    else
    {
        set* children = children_obj.data;
        if (!set_contains(children, &(obj){UInteger_t, &u_idx}))
        {
            crf_add_edge(con->CRF, v_idx, u_idx);

            // get all actions in P that start with (X, j)
            crf_action_head a = crf_action_head_struct(*X_idx, j);
            obj* h_set_obj = dict_get(con->P, &(obj){CRFActionHead_t, &a});
            if (h_set_obj != NULL)
            {
                set* h_set = h_set_obj->data;
                for (size_t k = 0; k < set_size(h_set); k++)
                {
                    // full action tuples are (X, j, h)
                    uint64_t* h = set_get_at_index(h_set, k)->data;
                    parser_descriptor_add(L, i, *h, con);
                    parser_bsr_add(L, i, j, *h, con);
                }
            }
        }
    }
}

/**
 * Insert a successfully parsed symbol into the set of BSRs.
 */
void parser_bsr_add(slot* L, uint64_t i, uint64_t j, uint64_t k, parser_context* con)
{
    vect* body = metaparser_get_production_body(L->head_idx, L->production_idx);
    if (vect_size(body) == L->dot)
    {
        // insert (head_idx, production_idx, i, j, k) into Y
        bsr_head b = new_prod_bsr_head_struct(L->head_idx, L->production_idx, i, k);
        parser_bsr_add_helper(&b, j, con);
    }
    else if (L->dot > 1)
    {
        // insert (s, i, j, k) into Y
        slice s = slice_struct(body, 0, L->dot);
        bsr_head b = new_str_bsr_head_struct(&s, i, k);
        parser_bsr_add_helper(&b, j, con);
    }
}

/**
 * Insert the BSR into the BSR set, and save any success BSRs encountered
 * Does not modify the original BSR b (it is copied).
 */
void parser_bsr_add_helper(bsr_head* b, uint64_t j, parser_context* con)
{
    obj* j_set_obj = dict_get(con->Y, &(obj){BSRHead_t, b});
    if (j_set_obj == NULL)
    {
        // insert the BSR head into Y, pointing to a new set of j values
        set* j_set = new_set();
        set_add(j_set, new_uint_obj(j));
        obj* b_obj = new_bsr_head_obj(bsr_head_copy(b));
        dict_set(con->Y, b_obj, new_set_obj(j_set));
    }
    else
    {
        // insert j into the j set if it doesn't already exist
        if (!set_contains(j_set_obj->data, &(obj){UInteger_t, &j})) { set_add(j_set_obj->data, new_uint_obj(j)); }
    }

    // check if the BSR is a root, i.e. for some α and l (and m if !con->whole), (S ::= α, 0, l, m) ∈ Υ
    if (b->type == prod_bsr && b->head_idx == con->start_idx && b->i == 0 && (!con->whole || b->k == con->m))
    {
        con->success = true;
    }
}

/**
 * Helper function to count the total number of elements in all first/follow sets.
 * fsets is either the array "metaparser_symbol_firsts" or "metaparser_symbol_follows"
 */
size_t parser_count_fsets_size(vect* fsets)
{
    size_t count = 0;
    for (size_t i = 0; i < vect_size(fsets); i++)
    {
        fset* s = vect_get(fsets, i)->data;
        count += set_size(s->terminals) + s->special;
    }
    return count;
}

/**
 * Compute all first sets for each symbol in the grammar
 */
void parser_compute_symbol_firsts()
{
    set* symbols = metaparser_get_symbols();

    // create empty fsets for each symbol in the grammar
    for (size_t i = 0; i < set_size(symbols); i++) { vect_append(parser_symbol_firsts, new_fset_obj(NULL)); }

    // compute firsts for all terminal symbols, since the fset is just the symbol itself.
    for (size_t symbol_idx = 0; symbol_idx < set_size(symbols); symbol_idx++)
    {
        if (!metaparser_is_symbol_terminal(symbol_idx)) { continue; }
        fset* symbol_fset = vect_get(parser_symbol_firsts, symbol_idx)->data;
        fset_add(symbol_fset, new_uint_obj(symbol_idx));
        symbol_fset->special = false;
    }

    // compute first for all non-terminal symbols. update each set until no new changes occur
    size_t count;
    do {
        // keep track of if any sets got larger (i.e. by adding new terminals to any fsets)
        count = parser_count_fsets_size(parser_symbol_firsts);

        // for each non-terminal symbol
        for (size_t symbol_idx = 0; symbol_idx < set_size(symbols); symbol_idx++)
        {
            if (metaparser_is_symbol_terminal(symbol_idx)) { continue; }

            fset* symbol_fset = vect_get(parser_symbol_firsts, symbol_idx)->data;
            set* bodies = metaparser_get_production_bodies(symbol_idx);
            for (size_t production_idx = 0; production_idx < set_size(bodies); production_idx++)
            {
                vect* body = metaparser_get_production_body(symbol_idx, production_idx);

                // for each element in body, get its fset, and merge into this one. stop if non-nullable
                for (size_t i = 0; i < vect_size(body); i++)
                {
                    uint64_t* body_symbol_idx = vect_get(body, i)->data;
                    fset* body_symbol_fset = vect_get(parser_symbol_firsts, *body_symbol_idx)->data;
                    fset_union_into(symbol_fset, fset_copy(body_symbol_fset), true);
                    if (!body_symbol_fset->special) { break; }
                }

                // epsilon strings add epsilon to fset
                if (vect_size(body) == 0) { symbol_fset->special = true; }
            }
        }
    } while (count < parser_count_fsets_size(parser_symbol_firsts));
}

/**
 * Compute all follow sets for each symbol in the grammar
 */
void parser_compute_symbol_follows()
{
    // steps for computing follow sets:
    // 1. place $ in FOLLOW(S) where S is the start symbol and $ is the input right endmarker
    // 2. If there is a production A -> αBβ, then everything in FIRST(β) except ϵ is in FOLLOW(B)
    // 3. if there is a production A -> αB, or a production A -> αBβ where FIRST(β) contains ϵ, then everything in
    //    FOLLOW(A) is in FOLLOW(B)

    set* symbols = metaparser_get_symbols();

    // first initialize fsets for each symbol in the grammar
    for (size_t i = 0; i < set_size(symbols); i++) { vect_append(parser_symbol_follows, new_fset_obj(NULL)); }

    // 1. add $ to the follow set of the start symbol
    uint64_t start_symbol_idx = metaparser_get_start_symbol_idx();
    ((fset*)vect_get(parser_symbol_follows, start_symbol_idx)->data)->special = true;

    // 2/3. add first of following substrings following terminals, and follow sets of rule heads
    dict* productions = metaparser_get_productions();
    size_t count;
    do {
        // keep track of if any sets got larger (i.e. by adding new terminals to any fsets)
        count = parser_count_fsets_size(parser_symbol_follows);

        for (size_t i = 0; i < dict_size(productions); i++)
        {
            obj head_idx_obj;
            obj bodies_set_obj;
            dict_get_at_index(productions, i, &head_idx_obj, &bodies_set_obj);
            uint64_t head_idx = *(uint64_t*)head_idx_obj.data;
            set* bodies = (set*)bodies_set_obj.data;

            // for each production body
            for (size_t body_idx = 0; body_idx < set_size(bodies); body_idx++)
            {
                vect* body = metaparser_get_production_body(head_idx, body_idx);

                // for each element in body, get its fset, and merge into this one. stop if non-nullable
                for (size_t i = 0; i < vect_size(body); i++)
                {
                    uint64_t* symbol_idx = vect_get(body, i)->data;

                    // create a substring beta of the body from i + 1 to the end, and compute its first set
                    slice beta = slice_struct(body, i + 1, vect_size(body));
                    fset* beta_first = parser_first_of_string(&beta);
                    bool nullable = beta_first->special; // save nullable status

                    // get union first of beta into the follow set of the symbol (ignoring epsilon)
                    fset* symbol_follow = vect_get(parser_symbol_follows, *symbol_idx)->data;
                    fset_union_into(symbol_follow, beta_first, false); // beta_first gets freed here

                    // if beta is nullable, add everything in follow set of head to follow set of the current terminal
                    if (nullable)
                    {
                        fset* head_follow = vect_get(parser_symbol_follows, head_idx)->data;
                        fset_union_into(symbol_follow, fset_copy(head_follow), true);
                    }
                }
            }
        }
    } while (count < parser_count_fsets_size(parser_symbol_follows));
}

/**
 * return the list of first sets for each symbol in the grammar.
 */
vect* parser_get_symbol_firsts() { return parser_symbol_firsts; }

/**
 * return the list of follow sets for each symbol in the grammar.
 */
vect* parser_get_symbol_follows() { return parser_symbol_follows; }

/**
 * return the first set for the given symbol
 */
fset* parser_first_of_symbol(uint64_t symbol_idx) { return vect_get(parser_symbol_firsts, symbol_idx)->data; }

/**
 * Compute the first set for the given string of symbols.
 *
 * symbol_first: vect<fset>
 * returned set needs to be freed when done.
 */
fset* parser_first_of_string(slice* string)
{
    fset* result = new_fset();
    vect* symbol_firsts = parser_get_symbol_firsts();

    if (slice_size(string) == 0)
    {
        // empty string is nullable
        result->special = true;
    }
    else
    {
        // handle each symbol in the string, until a non-nullable symbol is reached
        for (size_t i = 0; i < slice_size(string); i++)
        {
            uint64_t* symbol_idx = slice_get(string, i)->data;
            fset* first_i = vect_get(symbol_firsts, *symbol_idx)->data;
            bool nullable = first_i->special;
            fset_union_into(result, fset_copy(first_i),
                            false); // merge first of symbol into result. Don't merge nullable
            if (i == slice_size(string) - 1 && nullable) { result->special = true; }

            // only continue to next symbol if this symbol was nullable
            if (!nullable) { break; }
        }
    }

    return result;
}

/**
 * Memoized call to first of string. Returned fset is owned by the memoizer dict, and should not be freed.
 */
fset* parser_memo_first_of_string(slice* string)
{
    // check if the slice is in the dictionary already
    obj* result;
    if ((result = dict_get(parser_substring_firsts_dict, &(obj){.type = Slice_t, .data = string})) != NULL)
    {
        return result->data;
    }

    // otherwise, compute the first set and add it to the dictionary
    fset* result_fset = parser_first_of_string(string);
    dict_set(parser_substring_firsts_dict, new_slice_obj(slice_copy(string)), new_fset_obj(result_fset));

    return result_fset;
}

/**
 * return the follow set for the given symbol
 */
fset* parser_follow_of_symbol(uint64_t symbol_idx) { return vect_get(parser_symbol_follows, symbol_idx)->data; }

/**
 * print out the string of symbols in the given production body slice.
 */
void parser_print_body_slice(slice* body)
{
    if (slice_size(body) == 0) { printf("ϵ"); }
    for (size_t i = 0; i < slice_size(body); i++)
    {
        obj_str(metaparser_get_symbol(*(uint64_t*)(slice_get(body, i)->data)));
        if (i != slice_size(body) - 1) { printf(" "); }
    }
}

/**
 * print out the string of symbols for the given production body.
 */
void parser_print_body(vect* body)
{
    slice body_slice = slice_struct(body, 0, vect_size(body));
    parser_print_body_slice(&body_slice);
}

#endif