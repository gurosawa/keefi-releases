#pragma once
class QSplitter;

namespace keefi {
// A QSplitter keeps no width for a hidden child, so a side that is shown again
// can stay 0 px wide. Remember the second side's share of a two-sided splitter
// while both sides have width, and give it back within each side's floor.
class SplitterShare final {
  public:
    explicit SplitterShare(double secondShare) : share_(secondShare) {}
    double share() const { return share_; }
    // Width (or height) the two sides can share: the splitter minus its one handle.
    static int extent(const QSplitter&);
    // Keeps the previous share unless both sides currently have width.
    void remember(const QSplitter&);
    // false while the splitter has no width yet; nothing changes then. When
    // both floors cannot fit, the second side keeps its floor.
    bool restore(QSplitter&, int firstFloor, int secondFloor) const;

  private:
    double share_;
};
} // namespace keefi
