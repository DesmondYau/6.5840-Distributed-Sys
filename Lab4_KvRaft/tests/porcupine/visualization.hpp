// visualization.hpp
#pragma once

#include <iostream>
#include <string>
#include "checker.hpp"
#include "model.hpp"

namespace porcupine {

/**
 * @brief Logic to transform LinearizationInfo into the JSON format expected by the JS UI.
 */
std::string ComputeVisualizationJson(const Model& model, const LinearizationInfo& info);

/**
 * @brief Embeds the JSON data into the HTML template to an output stream.
 */
void Visualize(const Model& model, const LinearizationInfo& info, std::ostream& out);

/**
 * @brief Generates the visualization HTML file at the specified path.
 */
void VisualizePath(const Model& model, const LinearizationInfo& info, const std::string& path);

} // namespace porcupine