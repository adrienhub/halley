#pragma once
#include "../tasks/editor_task.h"
#include "import_assets_database.h"
#include "halley/file/directory_monitor.h"

namespace Halley
{
	class Project;

	class CheckAssetsTask : public EditorTask
	{
	public:
		CheckAssetsTask(Project& project, bool oneShot);

	protected:
		void run() override;

	private:
		Project& project;
		DirectoryMonitor monitorAssets;
		DirectoryMonitor monitorAssetsSrc;
		DirectoryMonitor monitorSharedAssetsSrc;
		DirectoryMonitor monitorGen;
		DirectoryMonitor monitorGenSrc;
		bool oneShot;

		static std::vector<ImportAssetsDatabaseEntry> filterNeedsImporting(ImportAssetsDatabase& db, const std::map<String, ImportAssetsDatabaseEntry>& assets);
		void checkAllAssets(ImportAssetsDatabase& db, std::vector<Path> srcPaths, Path dstPath, String taskName, bool packAfter);
		Maybe<Path> findDirectoryMeta(const std::vector<Path>& metas, const Path& path) const;
		bool importFile(ImportAssetsDatabase& db, std::map<String, ImportAssetsDatabaseEntry>& assets, const bool isCodegen, const std::vector<Path>& directoryMetas, const Path& srcPath, const Path& filePath);
	};
}
