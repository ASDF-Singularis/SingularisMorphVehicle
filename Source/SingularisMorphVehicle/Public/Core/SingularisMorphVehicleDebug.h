#pragma once

#include "Containers/StringConv.h"
#include "HAL/FileManager.h"

/**
 * 将字符串数组写入自定义报告文件。
 */
static bool WriteCustomReport(FString FileName, TArray<FString>& FileLines)
{
	bool ReportGenerated = false;

	if (FileLines.Num())
	{
		FString FullPath = FileName;
		FArchive* LogFile = IFileManager::Get().CreateFileWriter(
			*FullPath,
			FILEWRITE_NoReplaceExisting | FILEWRITE_Append
		);

		if (LogFile != nullptr)
		{
			for (auto Index = 0; Index < FileLines.Num(); ++Index)
			{
				FString LogEntry = FString::Printf(TEXT("%s"), *FileLines[Index]) + LINE_TERMINATOR;
				LogFile->Serialize(TCHAR_TO_ANSI(*LogEntry), LogEntry.Len());
			}

			LogFile->Close();
			delete LogFile;
			ReportGenerated = true;
		}
	}

	return ReportGenerated;
}

/**
 * 将网络调试日志行写入服务器/客户端文本报告文件。
 */
static bool WriteNetReport(bool IsServer, const FString& FileLine)
{
	FString FileName = IsServer ? "D:\\Server.txt" : "D:\\Client.txt";

	TArray<FString> FileLines;
	FileLines.Add(FileLine);
	return WriteCustomReport(FileName, FileLines);
}
