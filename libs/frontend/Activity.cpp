#include "frontend/Activity.h"

namespace Smith {

QStringList spinnerFrames(Theme theme) {
  if (theme == Theme::Plain) {
    return {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
  }
  // Half-width katakana, one column wide like the braille dots.
  QStringList frames;
  for (const QChar c : QString::fromUtf8("ﾊﾐﾋｰｳｼﾅﾓﾆｻﾜﾂｵﾘｱﾎﾃﾏｹﾒｴｶｷﾑﾕﾗｾﾈｽﾀﾇﾍ")) {
    frames << QString(c);
  }
  return frames;
}

QString runningLabel(const QStringList& running) {
  if (running.isEmpty()) {
    return "Thinking…";
  }
  if (running.size() == 1) {
    return running.first();
  }
  QStringList names;
  for (const QString& label : running) {
    names << label.section(' ', 0, 0);
  }
  return QString("%1 tools in parallel: %2").arg(running.size()).arg(names.join(", "));
}

} // namespace Smith
