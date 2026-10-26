#ifndef NDS_EXTRACT_HPP
#define NDS_EXTRACT_HPP
#include <string>

int SIM_NDSExtract(std::string ndsFilePath);
// Extracts into outputDir instead of the working directory
int SIM_NDSExtractTo(std::string ndsFilePath, std::string outputDir);

#endif
