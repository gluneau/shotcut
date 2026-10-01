/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mcpjobs.h"
#include "mcptools.h"
#include "mcptoolsutil.h"

using namespace McpTools;

namespace {

QJsonObject getJobTool(const QJsonObject &arguments)
{
    int id = 0;
    if (readFrame(arguments, "job_id", id) != FrameArg::Ok)
        return textResult("job_id must be a non-negative integer.", true);
    const auto record = McpJobs::find(id);
    if (!record)
        return textResult("No such job.", true);
    return jsonResult(McpJobs::statusJson(*record));
}

} // namespace

QList<McpTool> McpTools::analysisTools()
{
    return {
        {"get_job",
         "Status of a background analysis job (the tools that return {job_id}): {job_id, kind, "
         "state (running, succeeded, failed, cancelled), percent (when known), result (once "
         "succeeded), error (once failed), elapsed_ms}. elapsed_ms is the run time so far, or "
         "the total once finished. Jobs are kept in memory until Shotcut quits, also after the "
         "project is closed; they are not files on disk.",
         schema(R"({"type":"object","properties":{
             "job_id":{"type":"integer","minimum":0}
           },"required":["job_id"]})"),
         getJobTool},
    };
}
