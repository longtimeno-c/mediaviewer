// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The search agent's XPC interface (docs/design/23): the one surface between Final
// Cut Pro's workflow extension (sandboxed, in FCP's process tree) and the AI
// pack's reader (in the agent). Everything that crosses is NSData of the
// search_wire format or a plain string; the agent accepts a connection only
// from code signed by its own team (agent_mac.mm).
#import <Foundation/Foundation.h>

// Info.plist / launchd MachServices name; the extension's app group is its
// prefix, which is what lets a sandboxed client look it up.
#ifndef MV_FCP_MACH_SERVICE
#define MV_FCP_MACH_SERVICE "dev.mediaviewer.fcp.search"
#endif

@protocol MVSearchAgent
// `request`: search_wire encode(request). `text`: the query, or the file for
// find similar. `scopeDir`: "" for every indexed folder. The reply is
// search_wire encode(reply) and always arrives, with its status set when
// there are no rows.
- (void)run:(NSData*)request
        text:(NSString*)text
    scopeDir:(NSString*)scopeDir
   withReply:(void (^)(NSData* reply))reply;

// A tile's JPEG bytes: `path` must be one this agent handed out as a row's
// thumb (the extension's sandbox cannot read the viewer's cache). nil when it
// was not, or it has gone.
- (void)thumbnail:(NSString*)path withReply:(void (^)(NSData* jpeg))reply;

// The panel's scope picker: mv.ai.1 roots_json (the indexed folders), or nil
// while Local search is loading or unavailable.
- (void)rootsWithReply:(void (^)(NSString* json))reply;

// Completions for the word being typed: mv.ai.1 suggest_json (named people).
- (void)suggest:(NSString*)text withReply:(void (^)(NSString* json))reply;
@end
