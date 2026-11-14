#include "core/Tool.h"

#include "core/Json.h"
#include "core/Sandbox.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace Smith {

std::shared_ptr<Sandbox> ToolContext::sandboxOrNone() const {
  return sandbox ? sandbox : Sandbox::none();
}

QString plural(int count, const QString& singular, const QString& pluralForm) {
  const QString word = count == 1 ? singular : (pluralForm.isEmpty() ? singular + "s" : pluralForm);
  return QString("%1 %2").arg(count).arg(word);
}

namespace Schema {

QJsonObject string(const QString& description, int minLength) {
  QJsonObject schema{{"type", "string"}};
  if (!description.isEmpty()) {
    schema["description"] = description;
  }
  if (minLength > 0) {
    schema["minLength"] = minLength;
  }
  return schema;
}

QJsonObject integer(const QString& description, std::optional<int> minimum, std::optional<int> maximum) {
  QJsonObject schema{{"type", "integer"}};
  if (!description.isEmpty()) {
    schema["description"] = description;
  }
  if (minimum) {
    schema["minimum"] = *minimum;
  }
  if (maximum) {
    schema["maximum"] = *maximum;
  }
  return schema;
}

QJsonObject number(const QString& description) {
  QJsonObject schema{{"type", "number"}};
  if (!description.isEmpty()) {
    schema["description"] = description;
  }
  return schema;
}

QJsonObject boolean(const QString& description) {
  QJsonObject schema{{"type", "boolean"}};
  if (!description.isEmpty()) {
    schema["description"] = description;
  }
  return schema;
}

QJsonObject enumeration(const QStringList& values, const QString& description) {
  QJsonObject schema{{"type", "string"}, {"enum", QJsonArray::fromStringList(values)}};
  if (!description.isEmpty()) {
    schema["description"] = description;
  }
  return schema;
}

QJsonObject array(const QJsonObject& items, const QString& description, std::optional<int> maxItems) {
  QJsonObject schema{{"type", "array"}, {"items", items}};
  if (!description.isEmpty()) {
    schema["description"] = description;
  }
  if (maxItems) {
    schema["maxItems"] = *maxItems;
  }
  return schema;
}

QJsonObject object(const QList<QPair<QString, QJsonObject>>& properties, const QStringList& required) {
  QJsonObject props;
  for (const auto& [name, schema] : properties) {
    props[name] = schema;
  }
  QJsonObject schema{{"type", "object"}, {"properties", props}, {"additionalProperties", false}};
  if (!required.isEmpty()) {
    schema["required"] = QJsonArray::fromStringList(required);
  }
  return schema;
}

} // namespace Schema

namespace {

QString typeName(const QJsonValue& value) {
  switch (value.type()) {
  case QJsonValue::Null:
    return "null";
  case QJsonValue::Bool:
    return "boolean";
  case QJsonValue::Double:
    return "number";
  case QJsonValue::String:
    return "string";
  case QJsonValue::Array:
    return "array";
  case QJsonValue::Object:
    return "object";
  default:
    return "undefined";
  }
}

void validateInto(const QJsonValue& value, const QJsonObject& schema, const QString& path, QStringList& problems) {
  auto fail = [&](const QString& message) {
    problems << QString("✖ %1").arg(message) + (path.isEmpty() ? QString() : QString("\n  → at %1").arg(path));
  };
  const QString type = schema.value("type").toString();
  if (type == "string") {
    if (!value.isString()) {
      fail(QString("Invalid input: expected string, received %1").arg(typeName(value)));
      return;
    }
    const QString text = value.toString();
    const int minLength = schema.value("minLength").toInt(0);
    if (text.size() < minLength) {
      fail(QString("Too small: expected string to have >=%1 characters").arg(minLength));
    }
    if (schema.contains("enum")) {
      const QJsonArray allowed = schema.value("enum").toArray();
      if (!allowed.contains(text)) {
        QStringList names;
        for (const QJsonValue& option : allowed) {
          names << QString("\"%1\"").arg(option.toString());
        }
        fail(QString("Invalid option: expected one of %1").arg(names.join("|")));
      }
    }
  } else if (type == "integer" || type == "number") {
    if (!value.isDouble()) {
      fail(QString("Invalid input: expected number, received %1").arg(typeName(value)));
      return;
    }
    const double number = value.toDouble();
    if (type == "integer" && std::floor(number) != number) {
      fail("Invalid input: expected int, received number");
      return;
    }
    if (schema.contains("minimum") && number < schema.value("minimum").toDouble()) {
      fail(QString("Too small: expected number to be >=%1").arg(schema.value("minimum").toDouble()));
    }
    if (schema.contains("maximum") && number > schema.value("maximum").toDouble()) {
      fail(QString("Too big: expected number to be <=%1").arg(schema.value("maximum").toDouble()));
    }
  } else if (type == "boolean") {
    if (!value.isBool()) {
      fail(QString("Invalid input: expected boolean, received %1").arg(typeName(value)));
    }
  } else if (type == "array") {
    if (!value.isArray()) {
      fail(QString("Invalid input: expected array, received %1").arg(typeName(value)));
      return;
    }
    const QJsonArray items = value.toArray();
    if (schema.contains("maxItems") && items.size() > schema.value("maxItems").toInt()) {
      fail(QString("Too big: expected array to have <=%1 items").arg(schema.value("maxItems").toInt()));
    }
    const QJsonObject itemSchema = schema.value("items").toObject();
    for (int i = 0; i < items.size(); i++) {
      validateInto(items[i], itemSchema, path.isEmpty() ? QString("[%1]").arg(i) : QString("%1[%2]").arg(path).arg(i), problems);
    }
  } else if (type == "object") {
    if (!value.isObject()) {
      fail(QString("Invalid input: expected object, received %1").arg(typeName(value)));
      return;
    }
    const QJsonObject object = value.toObject();
    const QJsonObject properties = schema.value("properties").toObject();
    for (const QJsonValue& name : schema.value("required").toArray()) {
      if (!object.contains(name.toString())) {
        const QString at = path.isEmpty() ? name.toString() : path + "." + name.toString();
        QString expected = properties.value(name.toString()).toObject().value("type").toString();
        problems << QString("✖ Invalid input: expected %1, received undefined\n  → at %2").arg(expected, at);
      }
    }
    for (auto it = object.begin(); it != object.end(); ++it) {
      if (!properties.contains(it.key())) {
        // Unknown keys are ignored, as zod's default objects do.
        continue;
      }
      if (it.value().isNull() && !schema.value("required").toArray().contains(it.key())) {
        // Some models send null for optional fields; treat it as absent.
        continue;
      }
      validateInto(it.value(), properties.value(it.key()).toObject(), path.isEmpty() ? it.key() : path + "." + it.key(), problems);
    }
  }
}

} // namespace

std::optional<QString> validateSchema(const QJsonValue& value, const QJsonObject& schema) {
  QStringList problems;
  validateInto(value, schema, {}, problems);
  if (problems.isEmpty()) {
    return std::nullopt;
  }
  return problems.join("\n");
}

QJsonObject toApiTool(const ToolDef& tool) {
  return QJsonObject{
      {"type", "function"},
      {"function", QJsonObject{{"name", tool.name}, {"description", tool.description}, {"parameters", tool.schema}}},
  };
}

QString describeCall(const ToolList& tools, const QString& name, const QString& args) {
  for (const ToolDef& tool : tools) {
    if (tool.name != name) {
      continue;
    }
    try {
      const QJsonValue parsed = Json::parse(args.isEmpty() ? "{}" : args);
      if (!validateSchema(parsed, tool.schema)) {
        return tool.summarize(parsed.toObject());
      }
    } catch (const std::exception&) {
      // Fall through to the raw arguments.
    }
    break;
  }
  return truncateText(args, 200);
}

QString truncateText(const QString& text, int maxChars) {
  if (text.size() <= maxChars) {
    return text;
  }
  const int half = maxChars / 2;
  const int dropped = text.size() - 2 * half;
  return QString("%1\n\n[... %2 characters truncated ...]\n\n%3").arg(text.left(half)).arg(dropped).arg(text.right(half));
}

namespace {

// Lines that usually say what went wrong in build and test output.
const QRegularExpression PROBLEM(
    R"(\b(error|errors|failed|failure|fatal|exception|traceback|panic|undefined reference|cannot find|not found|no such file|segmentation fault|assert(ion)?)\b|^\s*(E|F)\s|✗|✖)",
    QRegularExpression::CaseInsensitiveOption);
const QRegularExpression NOT_A_PROBLEM(R"(\b0 (errors?|failed|failures)\b|-Werror|\berror(s)?: 0\b)", QRegularExpression::CaseInsensitiveOption);

} // namespace

QString truncateLog(const QString& text, int maxChars) {
  if (text.size() <= maxChars) {
    return text;
  }
  const int headBudget = maxChars / 5;
  const int tailBudget = int(maxChars * 0.4);
  const QStringList lines = text.split('\n');

  int head = 0;
  for (int used = 0; head < lines.size() && used + lines[head].size() + 1 <= headBudget; head++) {
    used += lines[head].size() + 1;
  }
  int tail = lines.size();
  for (int used = 0; tail > head && used + lines[tail - 1].size() + 1 <= tailBudget; tail--) {
    used += lines[tail - 1].size() + 1;
  }

  // Error lines from the middle, earliest first, until their budget runs out.
  const int middleBudget = maxChars - headBudget - tailBudget;
  QSet<int> kept;
  int used = 0;
  for (int i = head; i < tail && used < middleBudget; i++) {
    const QString& line = lines[i];
    if (!PROBLEM.match(line).hasMatch() || NOT_A_PROBLEM.match(line).hasMatch()) {
      continue;
    }
    for (int j = i; j < std::min(i + 3, tail); j++) {
      if (kept.contains(j)) {
        continue;
      }
      const int cost = std::min(int(lines[j].size()), 500) + 1;
      if (used + cost > middleBudget) {
        break;
      }
      kept.insert(j);
      used += cost;
    }
  }

  QStringList out = lines.mid(0, head);
  int gap = 0;
  auto flushGap = [&]() {
    if (gap) {
      out << QString("[... %1 omitted ...]").arg(plural(gap, "line"));
    }
    gap = 0;
  };
  for (int i = head; i < tail; i++) {
    if (kept.contains(i)) {
      flushGap();
      out << (lines[i].size() > 500 ? lines[i].left(500) + "…" : lines[i]);
    } else {
      gap++;
    }
  }
  flushGap();
  out << lines.mid(tail);
  const QString result = out.join('\n');
  // A few enormous lines can still blow the budget; fall back to plain cutting.
  return result.size() <= maxChars * 1.1 ? result : truncateText(result, maxChars);
}

} // namespace Smith
