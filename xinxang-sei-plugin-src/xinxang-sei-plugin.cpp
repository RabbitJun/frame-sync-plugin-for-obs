#include <obs-module.h>
#include <obs-frontend-api.h>

#include <util/platform.h>
#include <util/dstr.h>

#include <chrono>

#include <QAction>
#include <QBoxLayout>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QTimer>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QDateTime>
#include <QFileInfo>
#include <QTimeZone>

#include "xinxang-sei-inject.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("xinxang-sei", "en-US")

#define CFG_FILE "xinxang-sei.json"

static struct {
	struct xinxang_sei_config cfg;
	struct dstr client_name_storage;
	obs_output_t *attached_output;
	QAction *tools_action;
	QPointer<QDialog> settings_dialog;
	QPointer<QCheckBox> enabled_checkbox;
	QPointer<QLineEdit> client_name_edit;
	QPointer<QLabel> timestamp_label;
	QPointer<QTimer> timestamp_timer;
} xxs = {0};

static char *get_cfg_path(void)
{
	/* Ensure the module config directory exists */
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	return obs_module_config_path(CFG_FILE);
}

static void load_config(void)
{
	char *path = get_cfg_path();
	if (!path)
		return;

	obs_data_t *data = obs_data_create_from_json_file(path);
	if (!data)
		data = obs_data_create();

	obs_data_set_default_bool(data, "enabled", false);
	obs_data_set_default_bool(data, "only_rtmp_output", true);
	obs_data_set_default_string(data, "client_name", "RabbitJun");

	xxs.cfg.enabled = obs_data_get_bool(data, "enabled");
	xxs.cfg.only_rtmp_output = obs_data_get_bool(data, "only_rtmp_output");

	const char *name = obs_data_get_string(data, "client_name");
	dstr_copy(&xxs.client_name_storage, name ? name : "");
	dstr_depad(&xxs.client_name_storage);
	if (xxs.client_name_storage.len == 0)
		dstr_copy(&xxs.client_name_storage, "RabbitJun");

	xxs.cfg.client_name = xxs.client_name_storage.array;

	/* persist defaults on first run */
	obs_data_set_bool(data, "enabled", xxs.cfg.enabled);
	obs_data_set_bool(data, "only_rtmp_output", xxs.cfg.only_rtmp_output);
	obs_data_set_string(data, "client_name", xxs.cfg.client_name);
	obs_data_save_json_safe(data, path, ".tmp", ".bak");

	blog(LOG_INFO,
	     "[xinxang-sei] config loaded from '%s' (enabled=%s, only_rtmp=%s, client_name='%s')", path,
	     xxs.cfg.enabled ? "true" : "false", xxs.cfg.only_rtmp_output ? "true" : "false", xxs.cfg.client_name);

	obs_data_release(data);
	bfree(path);
}

static void save_config(bool enabled, const char *client_name)
{
	char *path = get_cfg_path();
	if (!path)
		return;

	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "enabled", enabled);
	obs_data_set_bool(data, "only_rtmp_output", true);
	obs_data_set_string(data, "client_name", client_name ? client_name : "");
	obs_data_save_json_safe(data, path, ".tmp", ".bak");
	obs_data_release(data);

	blog(LOG_INFO, "[xinxang-sei] config saved to '%s' (enabled=%s, client_name='%s')", path, enabled ? "true" : "false",
	     client_name ? client_name : "");

	bfree(path);
}

static void detach_from_output(void)
{
	if (!xxs.attached_output)
		return;

	blog(LOG_INFO, "[xinxang-sei] detaching packet callback from output '%s'",
	     obs_output_get_name(xxs.attached_output));
	obs_output_remove_packet_callback(xxs.attached_output, xinxang_sei_inject, &xxs.cfg);
	obs_output_release(xxs.attached_output);
	xxs.attached_output = NULL;
}

static void try_attach_to_streaming_output(void)
{
	if (!xxs.cfg.enabled) {
		blog(LOG_INFO, "[xinxang-sei] SEI injection disabled in config; skipping attach");
		return;
	}

	obs_output_t *out = obs_frontend_get_streaming_output();
	if (!out)
		return;

	obs_output_t *out_ref = obs_output_get_ref(out);
	if (!out_ref)
		return;

	if (xxs.cfg.only_rtmp_output) {
		const char *id = obs_output_get_id(out_ref);
		if (!id || strcmp(id, "rtmp_output") != 0) {
			obs_output_release(out_ref);
			return;
		}
	}

	/* avoid double-attach */
	if (xxs.attached_output == out_ref) {
		obs_output_release(out_ref);
		return;
	}

	detach_from_output();

	xxs.attached_output = out_ref;
	blog(LOG_INFO, "[xinxang-sei] attaching packet callback to output '%s' (id=%s), client_name='%s'",
	     obs_output_get_name(xxs.attached_output), obs_output_get_id(xxs.attached_output), xxs.cfg.client_name);
	obs_output_add_packet_callback(xxs.attached_output, xinxang_sei_inject, &xxs.cfg);
}

static int64_t utc_ms_now(void)
{
	using namespace std::chrono;
	return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static void update_timestamp_label(void)
{
	if (!xxs.timestamp_label)
		return;

	const int64_t ms = utc_ms_now();
	const QTimeZone tz = QTimeZone::fromSecondsAheadOfUtc(8 * 60 * 60);
	const QDateTime dt = QDateTime::fromMSecsSinceEpoch(ms, tz);
	xxs.timestamp_label->setText(dt.toString("yyyy-MM-dd HH:mm:ss.zzz"));
}

static QString try_get_logo_path(void)
{
	char *path = obs_module_file("YuJianLOGO1.png");
	if (path) {
		QString s = QString::fromUtf8(path);
		bfree(path);
		if (QFileInfo::exists(s))
			return s;
	}

	/* Local dev convenience: allow placing logo at F:\OBS\YuJianLOGO1.png */
	const QString fallback = QStringLiteral("F:\\OBS\\YuJianLOGO1.png");
	if (QFileInfo::exists(fallback))
		return fallback;

	return {};
}

static void ensure_settings_dialog(void)
{
	if (xxs.settings_dialog)
		return;

	QDialog *dlg = new QDialog(nullptr);
	dlg->setWindowTitle(QStringLiteral("帧同步插件"));
	dlg->setMinimumWidth(520);

	auto *root = new QVBoxLayout(dlg);
	root->setContentsMargins(18, 16, 18, 16);
	root->setSpacing(12);

	dlg->setStyleSheet(
		"QDialog { background: #171a20; color: #e8eaf0; }"
		"QLabel { color: #e8eaf0; }"
		"QLineEdit { background: #2a2f3a; border: 1px solid #3a4150; border-radius: 6px; padding: 8px 10px; color: #e8eaf0; }"
		"QCheckBox { spacing: 8px; }"
		"QCheckBox::indicator { width: 16px; height: 16px; }"
		"QDialogButtonBox QPushButton { background: #2a2f3a; border: 1px solid #3a4150; border-radius: 6px; padding: 8px 18px; }"
		"QDialogButtonBox QPushButton:hover { border-color: #5a6378; }"
		"QDialogButtonBox QPushButton:pressed { background: #232734; }");

	/* Header: logo + about text */
	auto *header = new QHBoxLayout();
	header->setSpacing(14);
	auto *logo = new QLabel(dlg);
	logo->setFixedHeight(48);
	logo->setMinimumWidth(180);
	logo->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

	const QString logo_path = try_get_logo_path();
	if (!logo_path.isEmpty()) {
		QPixmap pm(logo_path);
		if (!pm.isNull()) {
			const QPixmap scaled = pm.scaledToHeight(48, Qt::SmoothTransformation);
			logo->setPixmap(scaled);
		}
	}

	auto *aboutText = new QLabel(QStringLiteral("制作团队：丂渐工作室\n该插件遵循芯象SEI帧同步技术规范V1.0设计标准。"), dlg);
	aboutText->setWordWrap(true);
	aboutText->setStyleSheet("QLabel { color: #cfd5e3; }");

	header->addWidget(logo);
	header->addSpacing(12);
	header->addWidget(aboutText, 1);

	root->addLayout(header);
	root->addSpacing(12);

	/* Form: enabled + client_name */
	auto *form = new QFormLayout();
	form->setHorizontalSpacing(16);
	form->setVerticalSpacing(10);
	xxs.enabled_checkbox = new QCheckBox(QStringLiteral("启用"), dlg);
	xxs.enabled_checkbox->setChecked(xxs.cfg.enabled);

	xxs.client_name_edit = new QLineEdit(QString::fromUtf8(xxs.cfg.client_name ? xxs.cfg.client_name : "RabbitJun"), dlg);
	form->addRow(xxs.enabled_checkbox);
	form->addRow(QStringLiteral("客户端名字"), xxs.client_name_edit);

	root->addLayout(form);
	root->addSpacing(12);

	/* Timestamp */
	auto *tsRow = new QHBoxLayout();
	tsRow->setSpacing(10);
	auto *tsTitle = new QLabel(QStringLiteral("时间戳（UTC+8）:"), dlg);
	xxs.timestamp_label = new QLabel(dlg);
	xxs.timestamp_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
	xxs.timestamp_label->setStyleSheet("QLabel { color: #dfe3ee; font-weight: 600; }");
	tsRow->addWidget(tsTitle);
	tsRow->addWidget(xxs.timestamp_label, 1);
	root->addLayout(tsRow);

	xxs.timestamp_timer = new QTimer(dlg);
	QObject::connect(xxs.timestamp_timer, &QTimer::timeout, []() { update_timestamp_label(); });
	xxs.timestamp_timer->start(200);
	update_timestamp_label();

	/* Footer ad */
	auto *adLabel = new QLabel(
		QStringLiteral("推荐国人自研舞美中控系统 <a href=\"https://yujiancue.rbtstudio.cn/\">YuJianCue</a>"),
		dlg);
	adLabel->setObjectName(QStringLiteral("adLabel"));
	adLabel->setTextFormat(Qt::RichText);
	adLabel->setOpenExternalLinks(true);
	adLabel->setWordWrap(true);
	adLabel->setAlignment(Qt::AlignCenter);
	adLabel->setStyleSheet(
		"QLabel#adLabel { color: #9aa3b5; font-size: 12px; padding-top: 4px; }"
		"QLabel#adLabel a { color: #6ec1ff; text-decoration: none; }"
		"QLabel#adLabel a:hover { color: #9ad4ff; text-decoration: underline; }");
	root->addSpacing(8);
	root->addWidget(adLabel);

	/* Buttons */
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close, dlg);
	QObject::connect(buttons->button(QDialogButtonBox::Save), &QPushButton::clicked, [dlg]() {
		const bool enabled = xxs.enabled_checkbox ? xxs.enabled_checkbox->isChecked() : false;
		QString name = xxs.client_name_edit ? xxs.client_name_edit->text().trimmed() : QString();
		if (name.isEmpty())
			name = QStringLiteral("RabbitJun");

		save_config(enabled, name.toUtf8().constData());
		load_config(); /* refresh in-memory config */
		QMessageBox::information(dlg, QStringLiteral("帧同步插件"), QStringLiteral("已保存配置。重启推流后生效。"));
	});
	QObject::connect(buttons->button(QDialogButtonBox::Close), &QPushButton::clicked, dlg, &QDialog::close);
	root->addWidget(buttons);

	xxs.settings_dialog = dlg;
}

static void ensure_tools_menu_item(void)
{
	if (xxs.tools_action)
		return;

	xxs.tools_action = (QAction *)obs_frontend_add_tools_menu_qaction("帧同步插件");
	if (!xxs.tools_action)
		return;

	QObject::connect(xxs.tools_action, &QAction::triggered, []() {
		ensure_settings_dialog();
		if (xxs.settings_dialog) {
			xxs.settings_dialog->show();
			xxs.settings_dialog->raise();
			xxs.settings_dialog->activateWindow();
		}
	});
}

static void on_frontend_event(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		ensure_tools_menu_item();
		load_config();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		load_config();
		try_attach_to_streaming_output();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
	case OBS_FRONTEND_EVENT_EXIT:
		detach_from_output();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	load_config();
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	detach_from_output();
	dstr_free(&xxs.client_name_storage);
	xxs.tools_action = nullptr;
	xxs.settings_dialog = nullptr;
	xxs.enabled_checkbox = nullptr;
	xxs.client_name_edit = nullptr;
	xxs.timestamp_label = nullptr;
	xxs.timestamp_timer = nullptr;
}

