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

// get_editing_guide: static editing recipes that name the MCP tools to call, in order. Every
// step's tool and arguments_example must match tools/list (tools/mcp-smoke-format.sh checks it).

#include "mcpfilters.h"
#include "mcptools.h"
#include "mcptoolsutil.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QStringList>

using namespace McpTools;

namespace {

struct Step
{
    const char *tool;
    const char *arguments; // a JSON object
    const char *why;
};

struct Guide
{
    const char *useCase;
    const char *title;
    QList<Step> steps; // numbered from 1 by the tool
    const char *defaults; // a JSON object
    QStringList notes;
};

// Notes appended to every guide.
const QStringList kCommonNotes = {
    "Track 0 is the TOP video track; audio tracks follow the video tracks. Inserts, cuts, "
    "transitions, speed changes and track_add shift clip indexes (a new video track shifts every "
    "track index by one): re-read get_timeline, or use the tracks a tool returns, before the next "
    "call.",
    "Each tool call is ONE undo step: undo reverses the whole call; call undo again to go back "
    "further.",
    "Review with get_overview (a contact sheet of the whole timeline) and get_frame (one frame) "
    "before exporting; after export_video poll get_export_status.",
    "Frame numbers in arguments_example assume a 30 fps project; use your project's rate "
    "(get_timeline fps). Paths are placeholders.",
    "export_presets_list lists the presets export_video accepts; the output path must end in the "
    "preset's extension and must not be a file the project uses."
};

const Guide kGuides[] = {
    {"interview",
     "Interview or talking head: cut the pauses, level the voice, burn in captions",
     {
         {"analyze_media",
          R"({"path":"/path/to/interview.mp4"})",
          "Read the recording's duration, frame rate, size and audio streams first. The first "
          "file added sets the project's format unless project_set_format ran before."},
         {"transcribe",
          R"({"path":"/path/to/interview.mp4","wait_ms":20000})",
          "Whisper transcribes the whole file once; captions_from_transcript reuses it for every "
          "clip of this file, and its segments show what is said where. A long file returns "
          "{job_id}: poll get_job until it is done."},
         {"timeline_insert_range",
          R"({"path":"/path/to/interview.mp4","in_s":0,"out_s":1800,"track":0,"position":0})",
          "Put the whole recording on the timeline (out_s = its duration); the clip carries its "
          "sound. To keep only chosen answers instead, insert each speech range one after "
          "another (position = the previous clip's end) and skip the silence cuts."},
         {"audio_levels",
          R"({"track":0,"clip":0,"wait_ms":20000})",
          "Measure the voice (integrated loudness and peaks) before changing it."},
         {"clip_normalize",
          R"({"track":0,"clip":0,"target_lufs":-16,"wait_ms":20000})",
          "Bring the voice to -16 LUFS BEFORE cutting: every piece a cut leaves keeps the clip's "
          "filters, so one call covers them all. Let it finish (wait_ms, or get_job) before the "
          "next edit: the gain is applied to the clip as it was when measured."},
         {"detect_silence",
          R"({"track":0,"clip":0,"noise_db":-35,"min_s":0.6,"wait_ms":20000})",
          "Find pauses longer than 0.6 s below -35 dB; for a timeline clip every range also has "
          "timeline_frames to cut with."},
         {"save_project",
          R"({"path":"/path/to/interview-before-cuts.mlt"})",
          "Keep a copy of the project to go back to before many cuts."},
         {"timeline_remove_ranges",
          R"({"tracks":[0],"ranges":[{"start":905,"end":968},{"start":1510,"end":1592}],)"
          R"("ripple":true})",
          "Cut the pauses, each shrunk by 0.15 s (about 5 frames at 30 fps) at both ends so "
          "breaths and word endings stay. List every track that carries this recording (add the "
          "audio track's index when the sound is on its own track) so picture and sound stay in "
          "sync. At most 120 cuts (ranges times tracks) per call: for more, send chunks from the "
          "END of the timeline backwards."},
         {"get_overview",
          R"({})",
          "Look over the tightened edit; get_frame shows any single frame."},
         {"captions_from_transcript",
          R"({"track":0,"clip":0,"max_words":7})",
          "Burn in captions of at most 7 words from the transcript. It captions ONE clip per "
          "call and adds a subtitle track (Captions N) with its own burn-in each time: after the "
          "cuts, call it for every clip of track 0 (get_timeline lists them)."},
         {"export_video",
          R"({"path":"/path/to/interview-edit.mp4","preset":"x264-medium"})",
          "Render H.264 and AAC in an MP4 at the project's size and frame rate; poll "
          "get_export_status until it succeeds."},
         {"audio_levels",
          R"({"path":"/path/to/interview-edit.mp4","wait_ms":20000})",
          "Check the export: its integrated loudness should be -16 LUFS within 1.5 LU."}
     },
     R"({"silence_noise_db":-35,"silence_min_s":0.6,"silence_pad_s":0.15,)"
     R"("loudness_target_lufs":-16,"loudness_tolerance_lu":1.5,"caption_max_words":7,)"
     R"("export_preset":"x264-medium"})",
     {
         "detect_silence boundaries are approximate (about 0.1 s): keep the 0.15 s pads.",
         "Captions are times on the timeline and do not follow later edits: caption last, after "
         "every cut and move.",
         "Whisper's word times are approximate, so captions can lead or lag the speech by a few "
         "frames.",
         "Burned-in captions are part of the picture. Shotcut's Subtitles panel can also export a "
         "subtitle track as an .srt file."
     }},
    {"short",
     "Vertical short (9:16) from a longer landscape video",
     {
         {"project_set_format",
          R"({"preset":"9:16"})",
          "Make the project 1080x1920 BEFORE adding media; it is refused once anything is loaded "
          "(start a new project then)."},
         {"playlist_add",
          R"({"path":"/path/to/source.mp4"})",
          "Add the long source to the playlist (analyze_media gives its duration)."},
         {"transcribe",
          R"({"path":"/path/to/source.mp4","wait_ms":20000})",
          "The transcript helps find a 30 to 60 s moment that stands on its own (a question and "
          "its answer, a punchline), and the captions need it."},
         {"timeline_insert_range",
          R"({"playlist_index":0,"in_s":754.2,"out_s":801.0,"track":0,"position":0})",
          "Put only the chosen 30 to 60 s on the timeline."},
         {"get_frame",
          R"({"position":30,"max_width":640})",
          "See where the subject is across the wide picture to choose focus_x."},
         {"clip_reframe",
          R"({"track":0,"clip":0,"mode":"fill","focus_x":0.4})",
          "Crop the wide picture to fill 9:16 with the subject at the centre (focus_x 0 keeps "
          "the left edge, 1 the right edge); keyframes can follow a moving subject. Check with "
          "get_frame again."},
         {"clip_normalize",
          R"({"track":0,"clip":0,"target_lufs":-14,"wait_ms":20000})",
          "-14 LUFS suits phones and social platforms. Do it before the cuts so every piece "
          "keeps the gain."},
         {"detect_silence",
          R"({"track":0,"clip":0,"noise_db":-35,"min_s":0.4,"wait_ms":20000})",
          "Find dead air; a short is cut tighter than an interview."},
         {"timeline_remove_ranges",
          R"({"tracks":[0],"ranges":[{"start":212,"end":236}]})",
          "Cut the dead air (shrink each range by about 3 frames at both ends); ripple is on by "
          "default."},
         {"captions_from_transcript",
          R"({"track":0,"clip":0,"max_words":4,"style":{"size":120,"position":"middle"}})",
          "Few, large words read well on a phone, and the middle stays clear of the app's "
          "buttons. One clip per call (each makes a subtitle track): repeat for every clip the "
          "cuts left."},
         {"track_add",
          R"({"type":"video","name":"Hook"})",
          "A new top video track (index 0) for the hook title; the clip moves to track 1."},
         {"title_add",
          R"({"text":"Wait for the last line","track":0,"position":0,"duration":60,"size":110,)"
          R"("valign":"top","bg_color":"#99000000"})",
          "A hook for the first 2 s (60 frames at 30 fps) so viewers keep watching."},
         {"get_overview",
          R"({})",
          "Look over the short before rendering."},
         {"export_video",
          R"({"path":"/path/to/short.mp4","preset":"x264-medium"})",
          "Render the 1080x1920 MP4; poll get_export_status."}
     },
     R"({"format":"9:16","min_length_s":30,"max_length_s":60,"silence_noise_db":-35,)"
     R"("silence_min_s":0.4,"loudness_target_lufs":-14,"caption_max_words":4,)"
     R"("caption_size_px":120,"hook_title_s":2,"export_preset":"x264-medium"})",
     {
         "project_set_format works only on an empty project: call it first.",
         "clip_reframe fill crops the sides; mode pad shows the whole picture over a blurred copy "
         "of itself instead (no focus_x).",
         "Captions are times on the timeline and do not follow later edits: caption after the "
         "last cut.",
         "Phone apps draw buttons over the bottom and the right side of the picture: keep text "
         "away from them."
     }},
    {"tutorial",
     "Screen tutorial with the presenter's face in a corner",
     {
         {"project_set_format",
          R"({"preset":"16:9"})",
          "A 1920x1080 project, set BEFORE adding media."},
         {"timeline_insert_range",
          R"({"path":"/path/to/screen.mp4","in_s":0,"out_s":600,"track":0,"position":0})",
          "Put the screen recording on the timeline (with no timeline yet, track 0 creates it)."},
         {"track_add",
          R"({"type":"video","name":"Face"})",
          "A new top video track (index 0) for the camera; the screen moves to track 1."},
         {"timeline_insert_range",
          R"({"path":"/path/to/face.mp4","in_s":2.5,"out_s":602.5,"track":0,"position":0})",
          "Put the camera recording on track 0, trimmed with in_s so it is in sync with the "
          "screen (match a clap or a click heard in both)."},
         {"clip_set_transform",
          R"({"track":0,"clip":0,"scale":0.25,"x":672,"y":378})",
          "Shrink the face to a quarter and put it in the lower-right corner: on 1920x1080 the "
          "480x270 picture's centre moves 672 px right and 378 px down, leaving 48 px and 27 px "
          "margins."},
         {"transcribe",
          R"({"track":0,"clip":0,"wait_ms":20000})",
          "Transcribe the narration from the clip that carries the voice (the camera here)."},
         {"clip_normalize",
          R"({"track":0,"clip":0,"target_lufs":-16,"wait_ms":20000})",
          "Level the narration before the cuts. If the screen recording has sound too, lower it "
          "with clip_set_volume so the voice is not doubled."},
         {"detect_silence",
          R"({"track":0,"clip":0,"noise_db":-35,"min_s":0.8,"wait_ms":20000})",
          "Find the pauses in the narration; 0.8 s keeps short thinking pauses."},
         {"timeline_remove_ranges",
          R"({"tracks":[0,1],"ranges":[{"start":1500,"end":1590}]})",
          "Cut each pause on BOTH tracks so the face and the screen stay in sync (shrink each "
          "range by about 5 frames at both ends). Check with get_frame that no cut hides "
          "something the screen shows."},
         {"track_add",
          R"({"type":"video","name":"Titles"})",
          "A new top track for chapter titles; the face and the screen move to tracks 1 and 2."},
         {"lower_third_add",
          R"({"line1":"Step 1","line2":"Install the tools","track":0,"position":0,"duration":120})",
          "A chapter title across the bottom of the frame for 4 s at the start of each chapter."},
         {"marker_add",
          R"({"start":0,"text":"Step 1: Install the tools"})",
          "A marker at each chapter start; Shotcut can export markers as chapters for a video "
          "description."},
         {"captions_from_transcript",
          R"({"track":1,"clip":0})",
          "Captions from the narration clip (track 1 now), one call per clip the cuts left."},
         {"export_video",
          R"({"path":"/path/to/tutorial.mp4","preset":"x264-medium"})",
          "Render the MP4; poll get_export_status."}
     },
     R"({"format":"16:9","face_scale":0.25,"face_x":672,"face_y":378,"silence_noise_db":-35,)"
     R"("silence_min_s":0.8,"loudness_target_lufs":-16,"lower_third_frames":120,)"
     R"("export_preset":"x264-medium"})",
     {
         "The face, the lower thirds (76% to 90% of the height) and the captions all use the "
         "bottom of the frame: check with get_frame, and move the face up (clip_set_transform y "
         "-378) or the captions to the top (style position top) when they overlap.",
         "Always cut both tracks in one timeline_remove_ranges call; cutting one track at a time "
         "puts the face out of sync.",
         "Captions are times on the timeline and do not follow later edits: caption after the last "
         "cut."
     }},
    {"montage",
     "Montage of short shots cut to music",
     {
         {"analyze_media",
          R"({"path":"/path/to/clip1.mp4"})",
          "For each source: duration, frame rate, size and whether it has sound."},
         {"detect_scenes",
          R"({"path":"/path/to/clip1.mp4","threshold":0.3,"wait_ms":20000})",
          "The shot changes inside a source: take each shot from between them so no cut lands "
          "inside another cut."},
         {"detect_silence",
          R"({"path":"/path/to/music.mp3","noise_db":-30,"min_s":0.2,"wait_ms":20000})",
          "The music's quiet breaks, where a section changes and a cut feels natural. It does "
          "not find beats: to cut on the beat use the tempo (at 120 BPM a beat is 0.5 s, 15 "
          "frames at 30 fps) and make every shot a whole number of beats."},
         {"timeline_insert_range",
          R"({"path":"/path/to/clip1.mp4","in_s":12.0,"out_s":14.0,"track":0,"position":0})",
          "Each shot the same length (2 s here), placed at the end of the previous one (position "
          "0, 60, 120 and so on)."},
         {"clip_set_speed",
          R"({"track":0,"clip":3,"speed":0.5})",
          "Slow motion for an accent shot: speed 0.5 doubles its length and nothing else moves, "
          "so do it before the next shot is placed after it (or leave a gap for it)."},
         {"clip_set_color",
          R"({"track":0,"clip":0,"saturation":1.15,"temperature_k":6000,"contrast":0.05})",
          "The same look on every shot (one call per clip; calling it again updates the values "
          "rather than adding filters)."},
         {"timeline_add_transition",
          R"({"track":0,"clip":1,"duration":10})",
          "A short dissolve into a shot (clip is the incoming clip). Each transition overlaps "
          "two clips and makes the timeline shorter by its duration, so later clips move "
          "earlier: add them after the shots, speeds and colours, from the last clip backwards."},
         {"track_add",
          R"({"type":"audio","name":"Music"})",
          "An audio track for the music; it goes at the bottom (read its index from the result)."},
         {"timeline_insert_range",
          R"({"path":"/path/to/music.mp3","in_s":0,"out_s":60,"track":1,"position":0})",
          "The music under the shots, on the new audio track."},
         {"track_set_ducking",
          R"({"track":1,"threshold_db":-12,"reduction_db":-12})",
          "Dip the music while the tracks above it (shots with voices) are louder than -12 dB."},
         {"get_overview",
          R"({})",
          "Check the rhythm and the look of the whole montage."},
         {"export_video",
          R"({"path":"/path/to/montage.mp4","preset":"x264-medium"})",
          "Render the MP4; poll get_export_status."}
     },
     R"({"shot_length_s":2,"scene_threshold":0.3,"transition_frames":10,)"
     R"("ducking_threshold_db":-12,"ducking_reduction_db":-12,"slow_motion_speed":0.5,)"
     R"("export_preset":"x264-medium"})",
     {
         "track_set_ducking needs MLT 7.41 or newer (it is refused otherwise) and is refused on "
         "track 0.",
         "clip_set_color's saturation and temperature_k need MLT's frei0r plugins; without them "
         "the call is refused, while brightness and contrast still work.",
         "clip_set_speed refuses a clip next to a transition or with fades: change speeds before "
         "adding transitions.",
         "A transition's duration must be shorter than both clips it joins."
     }},
};


QJsonObject getEditingGuideTool(const QJsonObject &arguments)
{
    QString error;
    if (!McpFilters::checkKeys(arguments, {"use_case"}, error))
        return textResult(error, true);
    const QString useCase = arguments.value("use_case").toString();
    for (const Guide &guide : kGuides) {
        if (useCase != QLatin1String(guide.useCase))
            continue;
        QJsonArray steps;
        for (const Step &step : guide.steps) {
            steps.append(QJsonObject{
                {"n", steps.size() + 1},
                {"tool", QString::fromUtf8(step.tool)},
                {"why", QString::fromUtf8(step.why)},
                {"arguments_example", QJsonDocument::fromJson(step.arguments).object()}});
        }
        return jsonResult(QJsonObject{
            {"use_case", useCase},
            {"title", QString::fromUtf8(guide.title)},
            {"steps", steps},
            {"defaults", QJsonDocument::fromJson(guide.defaults).object()},
            {"notes", QJsonArray::fromStringList(guide.notes + kCommonNotes)}});
    }
    return textResult("use_case must be one of interview, short, tutorial, montage.", true);
}

} // namespace

QList<McpTool> McpTools::guideTools()
{
    return {
        {"get_editing_guide",
         "A step-by-step recipe for a common edit, naming the tools to call in order: use_case "
         "interview (cut pauses, level the voice, burn in captions), short (a 9:16 clip from a "
         "longer video), tutorial (a screen recording with the presenter's face in a corner) or "
         "montage (short shots cut to music). Returns {use_case, title, steps:[{n, tool, why, "
         "arguments_example}], defaults:{...}, notes:[...]}. The guide is fixed text: it reads "
         "nothing from the project, and arguments_example values (paths, frames for a 30 fps "
         "project, clip and track indexes) are examples to adapt; every tool and argument name "
         "is real. Nothing is changed.",
         schema(R"({"type":"object","properties":{
             "use_case":{"type":"string","enum":["interview","short","tutorial","montage"]}
           },"required":["use_case"],"additionalProperties":false})"),
         getEditingGuideTool},
    };
}
