# Widget object names (`lingtai_*`)

Stable `objectName` values for functional and visual UI tests. Prefer
`findChild<T *>(name)` over coordinates or sibling index.

C++ constants: [`object_names.h`](object_names.h)

## Shell and navigation

| objectName | Role |
| --- | --- |
| `lingtai_desktop_window` | Root window |
| `lingtai_desktop_body` | Main body |
| `lingtai_desktop_sidebar` | Left column (`AgentRoster` root) |
| `lingtai_desktop_content` | Right content column |
| `lingtai_startup_route` | First-run / choose project screen |
| `lingtai_startup_choose_project` | Choose project button |
| `lingtai_empty_workspace_route` | No project open state |
| `lingtai_open_project_button` | Open project (hidden seam; menu forwards here) |
| `lingtai_project_root` | Current project path label |
| `lingtai_project_selector` | Compact project selector |
| `lingtai_project_selector_menu` | Selector dropdown menu |

## Agent roster

| objectName | Role |
| --- | --- |
| `lingtai_agent_roster` | Roster panel inside sidebar |
| `lingtai_agent_roster_heading` | "Agents" heading |
| `lingtai_agent_roster_state` | Scan/status line |
| `lingtai_agent_roster_scroll` | Scroll area |
| `lingtai_agent_roster_rows` | Virtual row canvas (painted rows) |
| `lingtai_agent_directory` | Agent list + detail host |

**Row selection:** rows are painted on `lingtai_agent_roster_rows`, not
individual widgets. Use `AgentRoster::focus_row(key)` for keyboard focus, or
test helpers that click the canvas by row index. Do not use screen coordinates.

## Selected agent detail

| objectName | Role |
| --- | --- |
| `lingtai_agent_detail_scroll` | Detail scroll area |
| `lingtai_agent_detail` | Detail column root |
| `lingtai_chat_top_bar` | Selected agent header |
| `lingtai_selected_agent_avatar` | Avatar |
| `lingtai_selected_agent_presentation_name` | Display name |
| `lingtai_selected_agent_key` | Role / status line |
| `lingtai_agent_detail_back` | Narrow-mode back |
| `lingtai_selected_agent_start_agent` | Start agent |
| `lingtai_selected_agent_request_sleep` | Request sleep |
| `lingtai_agent_pages_nav` | Secondary page nav bar |
| `lingtai_agent_page_nav_conversation` | "← Conversation" |
| `lingtai_agent_page_nav_presets` | Presets tab (hidden on presets page) |
| `lingtai_agent_pages_host` | Presets / kanban host |
| `lingtai_selected_agent_conversation` | Conversation surface |
| `lingtai_selected_agent_conversation_state` | Conversation status |
| `lingtai_composer` | Composer lane |
| `lingtai_composer_input` | Message field |
| `lingtai_composer_send_button` | Send |
| `lingtai_composer_status` | Send status |
| `lingtai_selected_agent_preset_summary` | Preset catalog table |
| `lingtai_selected_agent_preset_summary_state` | Preset summary status |
| `lingtai_selected_agent_preset_summary_section` | Presets section container |
| `lingtai_kanban_page` | Kanban board page |

## Slash command popup

| objectName | Role |
| --- | --- |
| `lingtai_slash_command_card` | Popup card |
| `lingtai_slash_command_popup` | Command list |

## Project setup wizard

| objectName | Role |
| --- | --- |
| `lingtai_setup_preset_catalog` | Bootstrap preset list |
| `lingtai_setup_preset_search` | Preset search |
| `lingtai_setup_preset_continue` | Continue from presets |
| `lingtai_setup_edit_preset_page` | In-wizard preset editor |
| `lingtai_setup_edit_preset_save` | Save preset |
| `lingtai_setup_edit_preset_name` | Preset name field |
| `lingtai_setup_agents_page` | Agents step |
| `lingtai_setup_credentials_page` | Credentials step |

## Adding new names

Add `objectName` when introducing:

- buttons, inputs, toggles, lists users interact with
- major page/panel containers tests navigate to
- dynamic lists whose items tests must target

Skip static decorative labels unless tests must assert their text.
