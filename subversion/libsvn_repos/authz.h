/* authz.h : authz parsing and searching, private to libsvn_repos
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#ifndef SVN_REPOS_AUTHZ_H
#define SVN_REPOS_AUTHZ_H

#include <apr_hash.h>
#include <apr_pools.h>
#include <apr_tables.h>

#include "svn_config.h"
#include "svn_error.h"
#include "svn_io.h"
#include "svn_repos.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/*
 *   Authz and global group file parsing
 */

/* Number of (user, repository) combinations per authz for which we can
 * cache the corresponding filtered path rule trees.
 *
 * Since authz instance are per connection and there is usually only one
 * repository per connection, 2 (user + anonymous) would be sufficient in
 * most cases.  Having 4 adds plenty of headroom and we expect high locality
 * in any case.
 *
 * ### This number will be far too low if/when the parsed authz info
 *     becomes shared between multiple sessions.
 */
#define AUTHZ_FILTERED_CACHE_SIZE 4


/* A dictionary of rules that are specific to a particular
   (user, repository) combination. */
typedef struct authz_user_rules_t authz_user_rules_t;


/* Accumulated rights for (user, repository). */
typedef struct authz_rights_t
{
  /* The lowest level of access that the user has to every
     path in the repository. */
  svn_repos_authz_access_t min_access;

  /* The highest level of access that the user has to
     any path in the repository. */
  svn_repos_authz_access_t max_access;
} authz_rights_t;


/* Accumulated global rights for a specific user. */
typedef struct authz_global_rights_t
{
  /* The user name. */
  const char *user;

  /* Accumulated rights for this user across all repositories. */
  authz_rights_t all_repos_rights;

  /* Accumulated rights for this user from rules that are not
     repository-specific. We use this to avoid a hash lookup for the
     "any" repository rights. */
  authz_rights_t any_repos_rights;

  /* Accumulated rights for specific repositories.
     The key is repository name, the value is an authz_rights_t*. */
  apr_hash_t *per_repos_rights;
} authz_global_rights_t;


/* Immutable authorization info */
struct svn_authz_t
{
  /* All ACLs from the authz file, in the order of definition. */
  apr_array_header_t *acls;

  /* Globally accumulated rights for anonymous access. */
  svn_boolean_t has_anon_rights;
  authz_global_rights_t anon_rights;

  /* Globally accumulated rights for authenticated users. */
  svn_boolean_t has_authn_rights;
  authz_global_rights_t authn_rights;

  /* Globally accumulated rights, for all concrete users mentioned
     in the authz file. The key is the user name, the value is
     an authz_global_rights_t*. */
  apr_hash_t *user_rights;

  /* Fully recursively expanded group definitions, indexed by group name. */
  apr_hash_t *groups;

  /* A cache of rules filtered for a particular user.
     These will be generated on-demand. */
  authz_user_rules_t *user_rules[AUTHZ_FILTERED_CACHE_SIZE];

  /* The pool from which all the parsed authz data is allocated.
     This is the RESULT_POOL passed to svn_authz__tng_parse.

     It's a good idea to dedicate a pool for the authz structure, so
     that the whole authz representation can be deallocated by
     destroying the pool. */
  apr_pool_t *pool;
};


/* Rule path segment descriptor. */
typedef struct authz_rule_segment_t
{
  /* The segment type. */
  enum {
    /* A literal string match.
       The path segment must exactly match the pattern.

       Note: Make sure this is always the first constant in the
             enumeration, otherwise rules that match the repository
             root will not sort first in the ACL list and the implicit
             default no-access ACE will not be applied correctly. */
    authz_rule_literal,

    /* A prefix match: a literal string followed by '*'.
       The path segment must begin with the literal prefix. */
    authz_rule_prefix,

    /* A suffix match: '*' followed by a literal string.
       The path segment must end with the literal suffix.
       The pattern is stored reversed, so that the matching code can
       perform a prefix match on the reversed path segment. */
    authz_rule_suffix,

    /* '*'
       Matches any single non-empty path segment.
       The pattern will be an empty string. */
    authz_rule_any_segment,

    /* '**'
       Matches any sequence of zero or more path segments.
       The pattern will be an empty string. */
    authz_rule_any_recursive,

    /* Any other glob/fnmatch pattern. */
    authz_rule_fnmatch
  } kind;

  /* The pattern for this path segment.
     Any no-op fnmatch escape sequences (i.e., those that do not
     escape a wildcard or character class) are stripped from the
     string.

     The pattern string will be interned; therefore, two identical
     rule patterns will always contain the same pointer value and
     equality can therefore be tested by comparing the pointer
     values and segment kinds. */
  svn_string_t pattern;
} authz_rule_segment_t;

/* Rule path descriptor. */
typedef struct authz_rule_t
{
  /* The repository that this rule applies to. This will be the empty
     string string if a the rule did not name a repository. The
     repository name is interned. */
  const char *repos;

  /* The number of segments in the rule path. */
  int len;

  /* The array of path segments for this rule. Will be NULL for the
     repository root. */
  authz_rule_segment_t *path;
} authz_rule_t;


/* An access control list defined by access rules. */
typedef struct authz_acl_t
{
  /* The sequence number of the ACL stores the order in which access
     rules were defined in the authz file. The authz lookup code
     selects the highest-numbered ACL from amongst a set of equivalent
     matches. */
  int sequence_number;

  /* The parsed rule. */
  authz_rule_t rule;

  /* Access rights for anonymous users */
  svn_boolean_t has_anon_access;
  svn_repos_authz_access_t anon_access;

  /* Access rights for authenticated users */
  svn_boolean_t has_authn_access;
  svn_repos_authz_access_t authn_access;

  /* All other user- or group-specific access rights.
     Aliases are replaced with their definitions, rules for the same
     user or group are merged. */
  apr_array_header_t *user_access;
} authz_acl_t;


/* An access control entry in authz_acl_t::user_access. */
typedef struct authz_ace_t
{
  /* The name of the alias, user or group that this ACE applies to. */
  const char *name;

  /* The set of group members, when NAME is the name of a group.
     We store this reference in the ACE to save a hash lookup when
     resolving access for group ACEs.
   */
  apr_hash_t *members;

  /* True if this is an inverse-match rule. */
  svn_boolean_t inverted;

  /* The access rights defined by this ACE. */
  svn_repos_authz_access_t access;
} authz_ace_t;


/* Parse authz definitions from RULES and optional global group
 * definitions from GROUPS, returning an immutable, in-memory
 * representation of all the rules, groups and aliases.
 *
 * **AUTHZ and its contents will be allocated from RESULT_POOL.
 * The function uses SCRATCH_POOL for temporary allocations.
 */
svn_error_t *
svn_authz__parse(svn_authz_t **authz,
                 svn_stream_t *rules,
                 svn_stream_t *groups,
                 apr_pool_t *result_pool,
                 apr_pool_t *scratch_pool);


/* Reverse a STRING of length LEN in place. */
void
svn_authz__reverse_string(char *string, apr_size_t len);


/* Compare two rules in path lexical order, then repository lexical order. */
int
svn_authz__compare_rules(const authz_rule_t *a, const authz_rule_t *b);


/*
 *   Authorization lookup
 */

/* The "anonymous" user for authz queries. */
#define AUTHZ_ANONYMOUS_USER ((const char*)"")

/* Rules with this repository name apply to all repositories. */
#define AUTHZ_ANY_REPOSITORY ((const char*)"")


/* Check if the ACL applies to the (USER, REPOS) pair.  If it does,
 * and ACCESS is not NULL, set *ACCESS to the actual access rights for
 * the user in this repository.
 *
 * Use SCRATCH_POOL for temporary allocations.
 */
svn_boolean_t
svn_authz__acl_get_access(svn_repos_authz_access_t *access,
                          const authz_acl_t *acl,
                          const char *user, const char *repos);



#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_REPOS_AUTHZ_H */
