#pragma once

#include "core/Sandbox.h"

#include <QMap>
#include <QSet>

namespace Smith {

struct Mount {
  QString source;
  QString target;
  bool readOnly = false;
};

// "src", "src:ro", "src:dst" or "src:dst:ro", with ~ expanded; the target defaults to the same path.
Mount parseMount(const QString& spec);

// Runs commands in a container (podman or docker), one per project for the
// session, removed when Smith exits. The project is mounted read-write at the
// same path, its .git read-only on top; nothing else from this machine is
// visible except the configured mounts and skill folders.
class ContainerSandbox : public Sandbox {
public:
  ContainerSandbox(const ContainerSettings& settings, bool network, const std::optional<Limits>& limits, const QStringList& extraEnv);
  ~ContainerSandbox() override;

  SandboxType type() const override { return SandboxType::Container; }
  bool active() const override { return true; }
  bool network() const override;
  // A running container can't change its network: switching removes it, and the next command starts a new one.
  void setNetwork(bool on) override;

  QString describe(const QString& project = {}) const override;
  using Sandbox::spawnSpec;
  SpawnSpec spawnSpec(const QString& command, const QString& cwd, const CommandAccess& access) override;
  SpawnSpec programSpec(const QString& file, const QStringList& args, const QString& project, const QString& chdir = {}) override;
  QByteArray readBytes(const QString& file, const QString& project) override;
  void writeBytes(const QString& file, const QByteArray& content, const QString& project) override;
  std::optional<SandboxPath> sandboxPath(const QString& path, const QString& project) override;
  QStringList promptNotes(const QString& project) const override;
  std::optional<QString> explainFailure(const QString& output, const QString& project) const override;
  void dispose() override;

private:
  struct ProjectSettings {
    QString image;
    QStringList mounts;
  };

  ProjectSettings settingsFor(const QString& project) const;
  // The project's container, started on first use.
  QString container(const QString& project);
  QString start(const QString& project);
  QStringList envArgs() const;

  ContainerSettings m_settings;
  std::optional<Limits> m_limits;
  QStringList m_extraEnv;
  bool m_network;
  // Whether container root is safe to use: it maps to you, not to this machine's root.
  bool m_rootMapsToYou = true;
  QMap<QString, QString> m_containers;
  QSet<QString> m_started;
  // Serializes container starts, so parallel tools don't start two.
  QMutex m_startMutex;
};

} // namespace Smith
