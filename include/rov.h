#pragma once
#include "bgp.h"

/**
 * @brief ROV (Route Origin Validation) policy - extends BGP with security
 * 
 * ROV is a BGP security mechanism that defends against prefix hijacks.
 * When enabled, an AS will reject announcements marked as rov_invalid.
 * 
 * Real-World Context:
 * ==================
 * 
 * Without ROV (vulnerable):
 * - AS 666 announces "8.8.8.0/24" (Google's prefix)
 * - Other ASes accept it blindly
 * - Traffic intended for Google goes to AS 666 (hijack!)
 * 
 * With ROV (protected):
 * - AS 666 announces "8.8.8.0/24" with rov_invalid=true
 * - ROV-enabled ASes drop the announcement
 * - ROV-enabled ASes only route via legitimate AS 15169
 * 
 * How ROV Works:
 * ==============
 * 
 * 1. **RPKI (Resource Public Key Infrastructure)**
 *    - Database mapping prefixes to authorized origin ASes
 *    - Example: "8.8.8.0/24 is authorized for AS 15169"
 *    - Cryptographically signed by Regional Internet Registries
 * 
 * 2. **ROA (Route Origin Authorization)**
 *    - Certificate stating "prefix X can only be announced by AS Y"
 *    - Published in RPKI repositories
 * 
 * 3. **Validation**
 *    - When receiving announcement for prefix P from origin AS O:
 *      a) Check RPKI: Is AS O authorized for prefix P?
 *      b) If YES → rov_invalid = false (accept)
 *      c) If NO → rov_invalid = true (reject if ROV enabled)
 * 
 * Real-World Deployment:
 * ======================
 * 
 * As of 2025:
 * - ~40% of Internet ASes deploy ROV filtering
 * - Major networks: Cloudflare, Google, Amazon, AT&T
 * - Small ISPs: Often don't deploy (complexity, cost)
 * 
 * Impact:
 * - ROV ASes are protected from hijacks
 * - Non-ROV ASes remain vulnerable
 * - Mixed deployment creates interesting routing dynamics
 * 
 * Implementation Strategy:
 * ========================
 * 
 * We use **inheritance** to extend BGP with ROV functionality:
 * 
 * ```
 *     Policy (abstract)
 *        |
 *      BGP (base implementation)
 *        |
 *      ROV (adds filtering)
 * ```
 * 
 * Why inheritance?
 * - ROV is "BGP + filtering"
 * - Reuse all BGP route selection logic
 * - Only override receiveAnnouncement() to add filtering
 * - Clean separation of concerns
 * 
 * Alternative designs considered:
 * 1. Decorator pattern - more flexible but more complex
 * 2. Flag in BGP class - couples security with base routing
 * 3. Composition - ROV has-a BGP - more indirection
 * 
 * Performance:
 * ============
 * 
 * ROV overhead: O(1) per announcement
 * - Check rov_invalid flag: O(1)
 * - Drop if true: O(1)
 * - Otherwise call BGP::receiveAnnouncement(): O(k) where k = announcements for prefix
 * 
 * Memory overhead: None (reuses BGP's data structures)
 * 
 * Example Usage:
 * ==============
 * 
 * ```cpp
 * // Create AS with ROV protection
 * auto cloudflare = graph.getOrCreateNode(13335);
 * cloudflare->setPolicy(std::make_shared<ROV>());
 * 
 * // Legitimate announcement (not filtered)
 * Announcement google("8.8.8.0/24", 15169, false);  // rov_invalid=false
 * cloudflare->getPolicy()->receiveAnnouncement(google);
 * 
 * // Hijack attempt (filtered by ROV)
 * Announcement hijack("8.8.8.0/24", 666, true);  // rov_invalid=true
 * cloudflare->getPolicy()->receiveAnnouncement(hijack);  // DROPPED!
 * ```
 */
class ROV : public BGP {
public:
    /**
     * @brief Constructor - initializes ROV policy
     * 
     * Inherits all BGP functionality, adds ROV filtering.
     */
    ROV() = default;

    /**
     * @brief Receive and filter BGP announcement
     * 
     * Extends BGP::receiveAnnouncement() with ROV filtering:
     * 1. Check if announcement is marked rov_invalid
     * 2. If rov_invalid=true → DROP (don't store, don't forward)
     * 3. If rov_invalid=false → Pass to BGP::receiveAnnouncement()
     * 
     * @param ann The announcement to receive
     * 
     * Time Complexity: O(1) for filtering + O(k) for BGP processing
     * 
     * Security Properties:
     * - Prevents accepting hijacked routes
     * - Protects local routing table from poisoning
     * - Prevents forwarding invalid routes to neighbors
     * 
     * Example:
     * ```cpp
     * ROV rov;
     * 
     * // Legitimate route - accepted
     * Announcement legit("1.2.0.0/16", 777, false);
     * rov.receiveAnnouncement(legit);  // Stored in RIB
     * 
     * // Hijack - dropped
     * Announcement hijack("1.2.0.0/16", 666, true);
     * rov.receiveAnnouncement(hijack);  // SILENTLY DROPPED
     * ```
     * 
     * Why silent drop?
     * - Real ROV implementations log but don't alert sender
     * - Prevents feedback loops
     * - Hijacker doesn't know they're being filtered
     */
    void receiveAnnouncement(const Announcement& ann) override;
};
