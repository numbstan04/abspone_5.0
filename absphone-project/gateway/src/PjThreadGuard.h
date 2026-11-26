#pragma once

// pjlib requires every thread that calls into it to be registered first;
// pj_thread_this() asserts otherwise (pjlib/src/pj/os_core_win32.c). pj stack
// threads are registered by pj itself, but gateway's public entry points are
// also called from the host API thread (T1) and the EventBus dispatch thread
// (T2) — neither is a pj thread.
//
// Call this at the top of every GatewayManager entry that can reach a protocol
// adapter. The per-thread descriptor must stay alive for the thread's lifetime,
// hence thread_local storage.

#include <pj/os.h>
#include <pjsua-lib/pjsua.h>

namespace vcs {
	namespace gateway {

		inline void EnsurePjThreadRegistered() {
			if (pj_thread_is_registered()) {
				return;
			}
			static thread_local pj_thread_desc desc;
			static thread_local pj_thread_t* thread = nullptr;
			pj_bzero(desc, sizeof(desc));
			pj_thread_register("gateway_ext", desc, &thread);
		}

	} // namespace gateway
} // namespace vcs
