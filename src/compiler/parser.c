#ifndef PARSER_C
#define PARSER_C

#include <stdio.h>
#include <stdlib.h>
// #include <inttypes.h> // for PRIu64

#include "charset.h"
#include "metaparser.h"
#include "parser.h"
#include "set.h"

/**
 * Global data structures used by the parser
 * Slots are a head ::= rule, with a dot starting the rule, or following a non-terminals)
 */
vect* parser_labels;
// TODO->come up with better names for these globals
// set* P;
// set* Y;
// set* R;
// set* U;
uint32_t* I;
uint64_t cI;
uint64_t cU;
// CRF* crf;
// struct {
//     vect* labels;
//     set* P;
//     set* Y;
//     set* R;
//     set* U;
//     uint32_t* I;
//     uint64_t cI;
//     uint64_t cU;
//     // CRF* crf;
// } parser_context = {};

// Y, U, R, I, cI, cU, crf

void initialize_parser()
{
    parser_labels = new_vect();
    // other initializations here
}

void release_parser()
{
    vect_free(parser_labels);
    // other frees here
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
            for (s.position = 1; s.position <= vect_size(body); s.position++)
            {
                uint64_t* symbol_idx = vect_get(body, s.position - 1)->data;
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
void parser_handle_label(slot* label)
{
    // keep track of the current position in the item without modifying the original
    uint64_t dot = label->position;

    vect* body = metaparser_get_production_body(label->head_idx, label->production_idx);
    if (label->position == 0 && vect_size(body) == 0)
    {
        // Y.add((SubTerm(label.head, Sentence([])), cI, cI, cI))
    }
    else
    {
        while (dot < vect_size(body))
        {
            if (!metaparser_is_symbol_terminal(*(uint64_t*)vect_get(body, dot)->data)) { break; }
            if (dot != 0)
            {
                slice s = slice_struct(body, dot, vect_size(body), NULL);
                if (!parser_test_select(I[cI], label->head_idx, &s)) { return; }
            }
            dot++;

            parser_bsrAdd(slot_copy(label), cU, cI, cI + 1);
            cI++;
        }

        if (dot < vect_size(body))
        {
            if (dot != 0)
            {
                slice s = slice_struct(body, dot, vect_size(body), NULL);
                if (!parser_test_select(I[cI], label->head_idx, &s)) { return; }
            }
            dot++;
            parser_call(slot_copy(label), cU, cI);
        }
    }

    if (label->position == vect_size(body) ||
        (dot == vect_size(body) && metaparser_is_symbol_terminal(*(uint64_t*)vect_get(body, dot - 1)->data)))
    {
        // get the followset of the label head
        fset* follow = metaparser_follow_of_symbol(label->head_idx);
        if (fset_contains_c(follow, I[cI]))
        {
            parser_rtn(label->head_idx, cU, cI);
            return;
        }
    }
}

/**
 * print the CNP actions performed for the given label
 */
void parser_print_label(slot* label)
{
    slot_str(label);
    printf("\n");

    // keep track of the current position in the item without modifying the original
    uint64_t dot = label->position;

    vect* body = metaparser_get_production_body(label->head_idx, label->production_idx);
    if (label->position == 0 && vect_size(body) == 0)
    {
        printf("    Y.add((SubTerm(label.head, Sentence([])), cI, cI, cI))\n");
    }
    else
    {
        while (dot < vect_size(body))
        {
            if (!metaparser_is_symbol_terminal(*(uint64_t*)vect_get(body, dot)->data)) { break; }
            if (dot != 0)
            {
                slice s = slice_struct(body, dot, vect_size(body), NULL);
                printf("    if (!parser_test_select(I[cI], ");
                obj_str(metaparser_get_symbol(label->head_idx));
                printf(", ");
                metaparser_print_body_slice(&s);
                printf("))\n        goto L0\n");
            }
            dot++;
            printf("    parser_bsrAdd(");
            slot_str(&(slot){label->head_idx, label->production_idx, dot});
            printf(", cU, cI, cI + 1);\n    cI += 1\n");
        }

        if (dot < vect_size(body))
        {
            if (dot != 0)
            {
                slice s = slice_struct(body, dot, vect_size(body), NULL);
                printf("    if (!parser_test_select(I[cI], ");
                obj_str(metaparser_get_symbol(label->head_idx));
                printf(", ");
                metaparser_print_body_slice(&s);
                printf("))\n        goto L0\n");
            }
            dot++;
            printf("    parser_call(");
            slot_str(&(slot){label->head_idx, label->production_idx, dot});
            printf(", cU, cI);\n");
        }
    }

    if (label->position == vect_size(body) ||
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

bool parser_test_select(uint32_t c, uint64_t head, slice* string) { return false; }
void parser_bsrAdd(slot* slot, uint64_t i, uint64_t k, uint64_t j) {}
void parser_call(slot* slot, uint64_t i, uint64_t j) {}
void parser_rtn(uint64_t head, uint64_t k, uint64_t j) {}

#endif