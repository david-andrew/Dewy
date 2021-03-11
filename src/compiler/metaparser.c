#ifndef METAPARSER_C
#define METAPARSER_C

#include "vector.h"
#include "set.h"
#include "metatoken.h"


set* metaparser_heads;
set* metaparser_bodies;
set* metaparser_charsets;


/**
 * Initialize any internal objects used by the metaparser.
 */
void initialize_metaparser()
{
    metaparser_heads = new_set();
    metaparser_bodies = new_set();
    metaparser_charsets = new_set();
}


/**
 * Free any initialized objects used by the metaparser.
 */
void release_metaparser()
{
    #define safe_free(A) if(A) { set_free(A); A = NULL; }
    safe_free(metaparser_heads)
    safe_free(metaparser_bodies)
    safe_free(metaparser_charsets)
}



/**
 * Try to scan for a rule in the current list of tokens. 
 * Returns `true` if a rule was successfully parsed.
 */
bool parse_next_meta_rule(vect* tokens)
{
    //TODO
    return false;
}


// /**
//  * Return the index of the end of the next rule in the tokens vector
//  */
// size_t get_next_rule_boundary(vect* tokens, size_t start)
// {

// }



#endif