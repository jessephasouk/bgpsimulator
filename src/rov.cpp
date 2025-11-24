#include "rov.h"
#include <iostream>

/**
 * ROV (Route Origin Validation) announcement filtering
 * 
 * This is the core security feature of ROV:
 * - Check if announcement is marked as invalid
 * - If invalid: DROP (silently discard)
 * - If valid: Process normally through BGP
 * 
 * Real-World Analogy:
 * ===================
 * 
 * Think of ROV like airport security:
 * - Every passenger (announcement) goes through screening
 * - Invalid passport (rov_invalid=true) → denied entry (dropped)
 * - Valid passport (rov_invalid=false) → proceed to gate (BGP processing)
 * 
 * Why Silent Drop?
 * ================
 * 
 * ROV drops invalid announcements WITHOUT sending error messages:
 * 
 * 1. **Security** - Don't tell attacker they're being filtered
 * 2. **Scalability** - No feedback loops, no extra traffic
 * 3. **Standard** - RFC 6811 specifies silent drop behavior
 * 
 * What Happens to Dropped Announcements?
 * =======================================
 * 
 * - NOT stored in received queue
 * - NOT stored in local RIB
 * - NOT forwarded to neighbors
 * - Effectively, the announcement never existed for this AS
 * 
 * This is the key to ROV's effectiveness:
 * - Hijacked routes stop propagating through ROV-enabled ASes
 * - Creates "ROV-protected zones" in the Internet
 * 
 * Example Scenario:
 * =================
 * 
 * ```
 * AS 666 (attacker) announces 1.2.0.0/16 [rov_invalid=true]
 *     ↓
 * AS 2 (ROV-enabled)
 *     ↓ DROPPED! (never stored, never forwarded)
 * AS 3, AS 4, AS 5... (never see the hijack)
 * ```
 * 
 * Compare to Non-ROV AS:
 * ======================
 * 
 * ```
 * AS 666 (attacker) announces 1.2.0.0/16 [rov_invalid=true]
 *     ↓
 * AS 10 (NO ROV)
 *     ↓ ACCEPTED! (stored, forwarded)
 * AS 11, AS 12, AS 13... (all receive hijacked route)
 * ```
 * 
 * Mixed Deployment Dynamics:
 * ==========================
 * 
 * Internet has ~40% ROV deployment (as of 2025):
 * - Some ASes filter hijacks (protected)
 * - Some ASes don't filter (vulnerable)
 * - Creates complex routing patterns
 * - Our tests explore these scenarios
 * 
 * Performance:
 * ============
 * 
 * Time Complexity: O(1) for check + O(k) for BGP processing
 * - Check rov_invalid: O(1) (single bool)
 * - If valid, call BGP::receiveAnnouncement: O(k) where k = announcements
 * 
 * Best case (invalid): O(1) - immediate drop
 * Worst case (valid): O(k) - full BGP processing
 * 
 * Memory: Zero overhead
 * - Dropped announcements consume no memory
 * - Valid announcements use same memory as BGP
 * 
 * @param ann The announcement to filter and potentially process
 */
void ROV::receiveAnnouncement(const Announcement& ann) {
    // ROV filtering: Check if announcement is marked invalid
    if (ann.isROVInvalid()) {
        // Invalid announcement - DROP IT
        // 
        // No error message, no logging (in production would log)
        // Just silently discard to prevent hijack propagation
        // 
        // This is the security feature: invalid routes stop here
        return;  // Early return - announcement is discarded
    }
    
    // Valid announcement - process normally through BGP
    // 
    // This calls the parent class's implementation:
    // - Stores in received queue
    // - Runs route selection algorithm
    // - Updates local RIB if this is the best route
    BGP::receiveAnnouncement(ann);
}
