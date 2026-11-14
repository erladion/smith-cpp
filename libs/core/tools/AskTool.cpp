#include "core/tools/Tools.h"

#include <QJsonArray>

namespace Smith {

ToolDef askTool(std::function<Answer(const Question&)> ask) {
  ToolDef tool;
  tool.name = ASK_TOOL;
  tool.description = "Ask the user a question and wait for the answer. Use it when the request is ambiguous or needs a decision only the user can "
                     "make (which approach, which of several files, missing details), instead of guessing. Give 2-4 short options when there are "
                     "obvious choices; the user can always type their own answer. Don't ask about what you can find out by reading the code, and "
                     "don't ask for permission to run tools: the user is asked about those anyway.";
  tool.schema = Schema::object(
      {
          {"question", Schema::string("The question, ending with a question mark", 1)},
          {"options", Schema::array(Schema::string({}, 1), "Up to 4 suggested answers, a few words each", 4)},
      },
      {"question"});
  tool.readOnly = true;
  tool.summarize = [](const QJsonObject& input) { return input.value("question").toString(); };
  tool.run = [ask](const QJsonObject& input, const ToolContext&) {
    Question question{input.value("question").toString(), {}};
    for (const QJsonValue& option : input.value("options").toArray()) {
      question.options << option.toString();
    }
    const Answer answer = ask(question);
    switch (answer.kind) {
    case Answer::Kind::Dismissed:
      return ToolResult::error("The user dismissed the question without answering.");
    case Answer::Kind::Option:
      return ToolResult{"The user chose: " + question.options.value(answer.option)};
    case Answer::Kind::Text:
      return ToolResult{"The user answered: " + answer.text};
    }
    return ToolResult::error("The user dismissed the question without answering.");
  };
  return tool;
}

} // namespace Smith
