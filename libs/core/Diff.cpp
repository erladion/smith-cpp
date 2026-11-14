#include "core/Diff.h"

#include <QStringList>

#include <algorithm>
#include <vector>

namespace Smith {

namespace {

// Above this many changed lines on each side, skip the exact diff and show a
// plain replacement. Keeps memory bounded on huge rewrites.
constexpr int MAX_MYERS_LINES = 4000;

QStringList splitLines(const QString& text) {
  if (text.isEmpty()) {
    return {};
  }
  QStringList lines = text.split('\n');
  if (lines.last().isEmpty()) {
    lines.removeLast();
  }
  return lines;
}

// Myers' O((N+M)·D) shortest edit script: '=' keep, '-' remove from a, '+' add from b.
std::vector<char> myers(const QStringList& a, const QStringList& b) {
  const int n = a.size();
  const int m = b.size();
  const int max = n + m;
  const int offset = max;
  std::vector<int> v(2 * max + 2, 0);
  std::vector<std::vector<int>> trace;

  bool found = false;
  for (int d = 0; d <= max && !found; d++) {
    trace.push_back(v);
    for (int k = -d; k <= d; k += 2) {
      int x = (k == -d || (k != d && v[k - 1 + offset] < v[k + 1 + offset])) ? v[k + 1 + offset] : v[k - 1 + offset] + 1;
      int y = x - k;
      while (x < n && y < m && a[x] == b[y]) {
        x++;
        y++;
      }
      v[k + offset] = x;
      if (x >= n && y >= m) {
        found = true;
        break;
      }
    }
  }

  // Walk back through the saved states to recover the path.
  std::vector<char> ops;
  int x = n;
  int y = m;
  for (int d = int(trace.size()) - 1; d > 0; d--) {
    const std::vector<int>& vd = trace[d];
    const int k = x - y;
    const int prevK = (k == -d || (k != d && vd[k - 1 + offset] < vd[k + 1 + offset])) ? k + 1 : k - 1;
    const int prevX = vd[prevK + offset];
    const int prevY = prevX - prevK;
    while (x > prevX && y > prevY) {
      ops.push_back('=');
      x--;
      y--;
    }
    if (x == prevX) {
      ops.push_back('+');
      y--;
    } else {
      ops.push_back('-');
      x--;
    }
  }
  while (x > 0 && y > 0) {
    ops.push_back('=');
    x--;
    y--;
  }
  std::reverse(ops.begin(), ops.end());
  return ops;
}

} // namespace

Diff diffLines(const QString& before, const QString& after, int context) {
  const QStringList a = splitLines(before);
  const QStringList b = splitLines(after);

  // Common prefix and suffix are cheap to find and usually most of the file.
  int start = 0;
  while (start < a.size() && start < b.size() && a[start] == b[start]) {
    start++;
  }
  int endA = a.size();
  int endB = b.size();
  while (endA > start && endB > start && a[endA - 1] == b[endB - 1]) {
    endA--;
    endB--;
  }

  const QStringList middleA = a.mid(start, endA - start);
  const QStringList middleB = b.mid(start, endB - start);
  std::vector<char> ops;
  if (middleA.size() > MAX_MYERS_LINES || middleB.size() > MAX_MYERS_LINES) {
    ops.insert(ops.end(), middleA.size(), '-');
    ops.insert(ops.end(), middleB.size(), '+');
  } else {
    ops = myers(middleA, middleB);
  }

  // Full list of lines with numbers, then keep only changes and their context.
  Diff all;
  int x = 0;
  int y = 0;
  for (; x < start; x++, y++) {
    all.append({DiffLine::Kind::Context, a[x], x + 1, y + 1, 0});
  }
  for (char op : ops) {
    if (op == '=') {
      all.append({DiffLine::Kind::Context, a[x], x + 1, y + 1, 0});
      x++;
      y++;
    } else if (op == '-') {
      all.append({DiffLine::Kind::Remove, a[x], x + 1, 0, 0});
      x++;
    } else {
      all.append({DiffLine::Kind::Add, b[y], 0, y + 1, 0});
      y++;
    }
  }
  for (; x < a.size(); x++, y++) {
    all.append({DiffLine::Kind::Context, a[x], x + 1, y + 1, 0});
  }

  std::vector<bool> keep(all.size(), false);
  for (int i = 0; i < all.size(); i++) {
    if (all[i].kind == DiffLine::Kind::Context) {
      continue;
    }
    for (int j = std::max(0, i - context); j <= std::min(int(all.size()) - 1, i + context); j++) {
      keep[j] = true;
    }
  }

  Diff out;
  int skipped = 0;
  for (int i = 0; i < all.size(); i++) {
    if (!keep[i]) {
      skipped++;
      continue;
    }
    if (skipped > 0 && !out.isEmpty()) {
      DiffLine gap;
      gap.kind = DiffLine::Kind::Gap;
      gap.count = skipped;
      out.append(gap);
    }
    skipped = 0;
    out.append(all[i]);
  }
  return out;
}

DiffStats diffStats(const Diff& diff) {
  DiffStats stats;
  for (const DiffLine& line : diff) {
    if (line.kind == DiffLine::Kind::Add) {
      stats.added++;
    } else if (line.kind == DiffLine::Kind::Remove) {
      stats.removed++;
    }
  }
  return stats;
}

} // namespace Smith
