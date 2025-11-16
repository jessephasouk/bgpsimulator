#pragma once
#include "announcement.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include <optional>

/**
 * @brief Abstract base class for routing policies
 * 
 * The Policy class defines the interface for routing decision-making.
 * Different routing protocols (BGP, OSPF, etc.) can implement this interface
 * with their specific routing logic.
 * 
 * Design Pattern: Strategy Pattern
 * - Each AS has a Policy object
 * - Policy determines how to store and select routes
 * - Easy to swap routing protocols (BGP vs custom)
 * 
 * Why abstract class instead of concrete?
 * - Extensibility: Can implement different routing policies
 * - Testing: Can create mock policies for unit tests
 * - Research: Can experiment with BGP variants
 */
class Policy {
public:
    virtual ~Policy() = default;

    /**
     * @brief Process an incoming announcement
     * @param ann The announcement received from a neighbor
     * 
     * This is the main entry point for route updates.
     * Implementations should:
     * 1. Add to received queue
     * 2. Run route selection
     * 3. Update local RIB if better route found
     */
    virtual void receiveAnnouncement(const Announcement& ann) = 0;

    /**
     * @brief Get the best announcement for a prefix
     * @param prefix The IP prefix to look up
     * @return The best announcement, or std::nullopt if none exists
     * 
     * Returns the announcement currently in the local RIB.
     * This is the route the AS will actually use for forwarding.
     */
    virtual std::optional<Announcement> getBestAnnouncement(const IPPrefix& prefix) const = 0;

    /**
     * @brief Check if we have any route to a prefix
     * @param prefix The IP prefix to check
     * @return true if we have at least one announcement for this prefix
     */
    virtual bool hasRoute(const IPPrefix& prefix) const = 0;

    /**
     * @brief Get all received announcements for a prefix
     * @param prefix The IP prefix
     * @return Vector of all announcements received for this prefix
     * 
     * Used for debugging and testing route selection logic.
     */
    virtual std::vector<Announcement> getReceivedAnnouncements(const IPPrefix& prefix) const = 0;
};
