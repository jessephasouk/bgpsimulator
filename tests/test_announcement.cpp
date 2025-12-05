#include "announcement.h"
#include <gtest/gtest.h>
#include <stdexcept>

// ============================================================================
// IPPrefix Tests
// ============================================================================

class IPPrefixTest : public ::testing::Test {
protected:
    // Common test prefixes
    IPPrefix ipv4_prefix{"8.8.8.0/24"};
    IPPrefix ipv6_prefix{"2001:4860:4860::/48"};
};

TEST_F(IPPrefixTest, IPv4Parsing) {
    EXPECT_EQ(ipv4_prefix.toString(), "8.8.8.0/24");
    EXPECT_EQ(ipv4_prefix.getAddress(), "8.8.8.0");
    EXPECT_EQ(ipv4_prefix.getPrefixLength(), 24);
    EXPECT_TRUE(ipv4_prefix.isIPv4());
    EXPECT_FALSE(ipv4_prefix.isIPv6());
}

TEST_F(IPPrefixTest, IPv6Parsing) {
    EXPECT_EQ(ipv6_prefix.toString(), "2001:4860:4860::/48");
    EXPECT_EQ(ipv6_prefix.getAddress(), "2001:4860:4860::");
    EXPECT_EQ(ipv6_prefix.getPrefixLength(), 48);
    EXPECT_FALSE(ipv6_prefix.isIPv4());
    EXPECT_TRUE(ipv6_prefix.isIPv6());
}

TEST_F(IPPrefixTest, IPv4Variations) {
    IPPrefix prefix1("192.168.0.0/16");
    EXPECT_EQ(prefix1.getPrefixLength(), 16);
    
    IPPrefix prefix2("10.0.0.0/8");
    EXPECT_EQ(prefix2.getPrefixLength(), 8);
    
    IPPrefix prefix3("172.16.0.0/12");
    EXPECT_EQ(prefix3.getPrefixLength(), 12);
}

TEST_F(IPPrefixTest, IPv6Variations) {
    IPPrefix prefix1("2001:db8::/32");
    EXPECT_EQ(prefix1.getPrefixLength(), 32);
    EXPECT_TRUE(prefix1.isIPv6());
    
    IPPrefix prefix2("fe80::/10");
    EXPECT_EQ(prefix2.getPrefixLength(), 10);
}

TEST_F(IPPrefixTest, InvalidFormat) {
    // Missing slash
    EXPECT_THROW(IPPrefix("8.8.8.0"), std::invalid_argument);
    
    // Invalid prefix length
    EXPECT_THROW(IPPrefix("8.8.8.0/abc"), std::invalid_argument);
    
    // IPv4 prefix length too large
    EXPECT_THROW(IPPrefix("8.8.8.0/33"), std::invalid_argument);
    
    // IPv6 prefix length too large
    EXPECT_THROW(IPPrefix("2001:db8::/129"), std::invalid_argument);
}

TEST_F(IPPrefixTest, Comparison) {
    IPPrefix prefix1("8.8.8.0/24");
    IPPrefix prefix2("8.8.8.0/24");
    IPPrefix prefix3("8.8.4.0/24");
    
    EXPECT_EQ(prefix1, prefix2);
    EXPECT_NE(prefix1, prefix3);
    EXPECT_TRUE(prefix3 < prefix1);  // Lexicographic comparison
}

// ============================================================================
// Announcement Tests
// ============================================================================

class AnnouncementTest : public ::testing::Test {
protected:
    IPPrefix google_dns{"8.8.8.0/24"};
    IPPrefix google_ipv6{"2001:4860::/32"};
    uint32_t google_asn = 15169;
    uint32_t level3_asn = 3356;
    uint32_t verizon_asn = 701;
};

TEST_F(AnnouncementTest, OriginAnnouncement) {
    // Google announces its DNS range
    Announcement ann(google_dns, google_asn);
    
    EXPECT_EQ(ann.getPrefix(), google_dns);
    EXPECT_EQ(ann.getPathLength(), 1);
    EXPECT_EQ(ann.getASPath()[0], google_asn);
    EXPECT_EQ(ann.getOriginASN(), google_asn);
    EXPECT_EQ(ann.getNextHop(), google_asn);
    EXPECT_EQ(ann.getReceivedFrom(), RelationshipType::ORIGIN);
}

TEST_F(AnnouncementTest, IPv6Announcement) {
    // Google announces IPv6 range
    Announcement ann(google_ipv6, google_asn);
    
    EXPECT_EQ(ann.getPrefix(), google_ipv6);
    EXPECT_TRUE(ann.getPrefix().isIPv6());
    EXPECT_EQ(ann.getOriginASN(), google_asn);
}

TEST_F(AnnouncementTest, FullConstructor) {
    uint32_t path[] = {level3_asn, google_asn};
    Announcement ann(google_dns, path, 2, level3_asn, RelationshipType::FROM_CUSTOMER);
    
    EXPECT_EQ(ann.getPrefix(), google_dns);
    EXPECT_EQ(ann.getPathLength(), 2);
    EXPECT_EQ(ann.getASPath()[0], level3_asn);
    EXPECT_EQ(ann.getASPath()[1], google_asn);
    EXPECT_EQ(ann.getOriginASN(), google_asn);
    EXPECT_EQ(ann.getNextHop(), level3_asn);
    EXPECT_EQ(ann.getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
}

TEST_F(AnnouncementTest, LoopDetection) {
    // Create announcement with path: [3356, 15169]
    uint32_t path[] = {level3_asn, google_asn};
    Announcement ann(google_dns, path, 2, level3_asn, RelationshipType::FROM_CUSTOMER);
    
    // Check if ASNs are in path
    EXPECT_TRUE(ann.containsASN(google_asn));
    EXPECT_TRUE(ann.containsASN(level3_asn));
    EXPECT_FALSE(ann.containsASN(verizon_asn));
    EXPECT_FALSE(ann.containsASN(12345));
}

TEST_F(AnnouncementTest, PrependASN) {
    // Start with Google's announcement
    Announcement original(google_dns, google_asn);
    
    // Level3 receives and prepends
    Announcement level3_ann = original.prependASN(
        level3_asn, 
        level3_asn, 
        RelationshipType::FROM_CUSTOMER
    );
    
    EXPECT_EQ(level3_ann.getPathLength(), 2);
    EXPECT_EQ(level3_ann.getASPath()[0], level3_asn);
    EXPECT_EQ(level3_ann.getASPath()[1], google_asn);
    EXPECT_EQ(level3_ann.getOriginASN(), google_asn);
    EXPECT_EQ(level3_ann.getNextHop(), level3_asn);
    EXPECT_EQ(level3_ann.getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
    
    // Verizon receives from Level3 and prepends
    Announcement verizon_ann = level3_ann.prependASN(
        verizon_asn,
        verizon_asn,
        RelationshipType::FROM_PROVIDER
    );
    
    EXPECT_EQ(verizon_ann.getPathLength(), 3);
    EXPECT_EQ(verizon_ann.getASPath()[0], verizon_asn);
    EXPECT_EQ(verizon_ann.getASPath()[1], level3_asn);
    EXPECT_EQ(verizon_ann.getASPath()[2], google_asn);
    EXPECT_EQ(verizon_ann.getOriginASN(), google_asn);
    EXPECT_EQ(verizon_ann.getNextHop(), verizon_asn);
}

TEST_F(AnnouncementTest, PrependDoesNotModifyOriginal) {
    // Verify immutability
    Announcement original(google_dns, google_asn);
    size_t original_length = original.getPathLength();
    
    // Prepend creates a new announcement
    Announcement modified = original.prependASN(
        level3_asn,
        level3_asn,
        RelationshipType::FROM_CUSTOMER
    );
    
    // Original should be unchanged
    EXPECT_EQ(original.getPathLength(), original_length);
    EXPECT_EQ(original.getPathLength(), 1);
    EXPECT_EQ(original.getASPath()[0], google_asn);
    
    // Modified should be different
    EXPECT_EQ(modified.getPathLength(), original_length + 1);
    EXPECT_EQ(modified.getASPath()[0], level3_asn);
}

TEST_F(AnnouncementTest, ToString) {
    Announcement ann(google_dns, google_asn);
    std::string str = ann.toString();
    
    // Should contain prefix
    EXPECT_NE(str.find("8.8.8.0/24"), std::string::npos);
    
    // Should contain ASN
    EXPECT_NE(str.find("15169"), std::string::npos);
    
    // Should contain relationship
    EXPECT_NE(str.find("origin"), std::string::npos);
}

TEST_F(AnnouncementTest, ToStringWithPath) {
    uint32_t path[] = {verizon_asn, level3_asn, google_asn};
    Announcement ann(google_dns, path, 3, verizon_asn, RelationshipType::FROM_PROVIDER);
    std::string str = ann.toString();
    
    // Should contain all ASNs
    EXPECT_NE(str.find("701"), std::string::npos);
    EXPECT_NE(str.find("3356"), std::string::npos);
    EXPECT_NE(str.find("15169"), std::string::npos);
    
    // Should contain relationship
    EXPECT_NE(str.find("provider"), std::string::npos);
}

TEST_F(AnnouncementTest, Equality) {
    Announcement ann1(google_dns, google_asn);
    Announcement ann2(google_dns, google_asn);
    
    EXPECT_EQ(ann1, ann2);
    
    // Different prefix
    IPPrefix different_prefix("8.8.4.0/24");
    Announcement ann3(different_prefix, google_asn);
    EXPECT_NE(ann1, ann3);
    
    // Different ASN
    Announcement ann4(google_dns, level3_asn);
    EXPECT_NE(ann1, ann4);
}

TEST_F(AnnouncementTest, DifferentRelationships) {
    uint32_t path[] = {level3_asn, google_asn};
    
    Announcement from_customer(google_dns, path, 2, level3_asn, RelationshipType::FROM_CUSTOMER);
    Announcement from_peer(google_dns, path, 2, level3_asn, RelationshipType::FROM_PEER);
    Announcement from_provider(google_dns, path, 2, level3_asn, RelationshipType::FROM_PROVIDER);
    
    EXPECT_EQ(from_customer.getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
    EXPECT_EQ(from_peer.getReceivedFrom(), RelationshipType::FROM_PEER);
    EXPECT_EQ(from_provider.getReceivedFrom(), RelationshipType::FROM_PROVIDER);
    
    // Different relationships make announcements different
    EXPECT_NE(from_customer, from_peer);
    EXPECT_NE(from_peer, from_provider);
}

// ============================================================================
// RelationshipType Tests
// ============================================================================

TEST(RelationshipTypeTest, ToString) {
    EXPECT_EQ(relationshipToString(RelationshipType::FROM_CUSTOMER), "customer");
    EXPECT_EQ(relationshipToString(RelationshipType::FROM_PEER), "peer");
    EXPECT_EQ(relationshipToString(RelationshipType::FROM_PROVIDER), "provider");
    EXPECT_EQ(relationshipToString(RelationshipType::ORIGIN), "origin");
}

// ============================================================================
// Integration Test: Realistic BGP Scenario
// ============================================================================

TEST(AnnouncementIntegration, RealisticPropagation) {
    // Simulate: Google announces 8.8.8.0/24
    // Path: Google → Level3 → Verizon → Small ISP
    
    IPPrefix google_dns("8.8.8.0/24");
    uint32_t google = 15169;
    uint32_t level3 = 3356;
    uint32_t verizon = 701;
    uint32_t small_isp = 12345;
    
    // Step 1: Google originates
    Announcement google_ann(google_dns, google);
    EXPECT_EQ(google_ann.getPathLength(), 1);
    EXPECT_FALSE(google_ann.containsASN(level3));
    
    // Step 2: Level3 receives from customer Google
    Announcement level3_ann = google_ann.prependASN(
        level3,
        level3,
        RelationshipType::FROM_CUSTOMER
    );
    EXPECT_EQ(level3_ann.getPathLength(), 2);
    EXPECT_TRUE(level3_ann.containsASN(google));
    EXPECT_TRUE(level3_ann.containsASN(level3));
    EXPECT_EQ(level3_ann.getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
    
    // Step 3: Verizon receives from peer Level3
    Announcement verizon_ann = level3_ann.prependASN(
        verizon,
        verizon,
        RelationshipType::FROM_PEER
    );
    EXPECT_EQ(verizon_ann.getPathLength(), 3);
    EXPECT_EQ(verizon_ann.getReceivedFrom(), RelationshipType::FROM_PEER);
    
    // Step 4: Small ISP receives from provider Verizon
    Announcement isp_ann = verizon_ann.prependASN(
        small_isp,
        small_isp,
        RelationshipType::FROM_PROVIDER
    );
    EXPECT_EQ(isp_ann.getPathLength(), 4);
    EXPECT_EQ(isp_ann.getASPath()[0], small_isp);
    EXPECT_EQ(isp_ann.getASPath()[1], verizon);
    EXPECT_EQ(isp_ann.getASPath()[2], level3);
    EXPECT_EQ(isp_ann.getASPath()[3], google);
    EXPECT_EQ(isp_ann.getOriginASN(), google);
    EXPECT_EQ(isp_ann.getReceivedFrom(), RelationshipType::FROM_PROVIDER);
    
    // Verify loop detection works
    EXPECT_TRUE(isp_ann.containsASN(google));
    EXPECT_TRUE(isp_ann.containsASN(level3));
    EXPECT_TRUE(isp_ann.containsASN(verizon));
    EXPECT_TRUE(isp_ann.containsASN(small_isp));
    EXPECT_FALSE(isp_ann.containsASN(99999));
}
