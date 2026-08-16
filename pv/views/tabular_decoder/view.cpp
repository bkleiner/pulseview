/*
 * This file is part of the PulseView project.
 *
 * Copyright (C) 2020 Soeren Apel <soeren@apelpie.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <climits>

#include <QApplication>
#include <QDebug>
#include <QFileDialog>
#include <QFontMetrics>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QShortcut>
#include <QToolBar>
#include <QVBoxLayout>

#include <libsigrokdecode/libsigrokdecode.h>

#include "view.hpp"

#include "pv/globalsettings.hpp"
#include "pv/session.hpp"
#include "pv/util.hpp"
#include "pv/data/decode/decoder.hpp"

using pv::data::DecodeSignal;
using pv::data::SignalBase;
using pv::data::decode::Decoder;
using pv::util::Timestamp;

using std::make_shared;
using std::max;
using std::shared_ptr;

namespace pv {
namespace views {
namespace tabular_decoder {

namespace {

class MultiSelectMenu : public QMenu
{
public:
	using QMenu::QMenu;

protected:
	void mouseReleaseEvent(QMouseEvent *event) override
	{
		QAction *const action = actionAt(event->position().toPoint());
		if (action && action->isEnabled() && action->isCheckable()) {
			action->setChecked(!action->isChecked());
			return;
		}

		QMenu::mouseReleaseEvent(event);
	}
};

QString filter_button_text(const QString &label, qsizetype selected,
	qsizetype available)
{
	if (selected == available)
		return label + QStringLiteral(": ") + QObject::tr("All");
	if (selected == 0)
		return label + QStringLiteral(": ") + QObject::tr("None");
	return QStringLiteral("%1: %2/%3").arg(label).arg(selected).arg(available);
}

} // namespace

const char* SaveTypeNames[SaveTypeCount] = {
	"CSV, commas escaped",
	"CSV, fields quoted"
};

const char* ViewModeNames[ViewModeCount] = {
	"Show all",
	"Show all and focus on newest",
	"Show visible in main view"
};


CustomFilterProxyModel::CustomFilterProxyModel(QObject* parent) :
	QSortFilterProxyModel(parent),
	range_filtering_enabled_(false),
	group_filtering_enabled_(false),
	type_filtering_enabled_(false)
{
}

bool CustomFilterProxyModel::filterAcceptsRow(int sourceRow,
	const QModelIndex &sourceParent) const
{
	assert(sourceModel() != nullptr);
	const AnnotationCollectionModel *const annotation_model =
		qobject_cast<const AnnotationCollectionModel*>(sourceModel());
	const QModelIndex source_index = sourceModel()->index(sourceRow, 0, sourceParent);
	const Annotation *const ann = annotation_model ?
		static_cast<const Annotation*>(source_index.internalPointer()) : nullptr;

	bool result = true;

	if (range_filtering_enabled_) {
		const uint64_t ann_start_sample = ann ? ann->start_sample() :
			sourceModel()->data(source_index, Qt::DisplayRole).toULongLong();
		const uint64_t ann_end_sample = ann ? ann->end_sample() :
			sourceModel()->data(sourceModel()->index(sourceRow, 6, sourceParent),
				Qt::DisplayRole).toULongLong();

		// We consider all annotations as visible that either
		// a) begin to the left of the range and end within the range or
		// b) begin and end within the range or
		// c) begin within the range and end to the right of the range
		// ...which is equivalent to the negation of "begins and ends outside the range"

		const bool left_of_range = (ann_end_sample < range_start_sample_);
		const bool right_of_range = (ann_start_sample > range_end_sample_);
		const bool entirely_outside_of_range = left_of_range || right_of_range;

		result = !entirely_outside_of_range;
	}

	if (!result)
		return result;

	if (group_filtering_enabled_) {
		const quintptr group_id = ann ? reinterpret_cast<quintptr>(ann->row()) :
			sourceModel()->data(source_index, AnnotationGroupIdRole).toULongLong();
		if (!group_filter_.contains(group_id))
			return false;
	}

	if (type_filtering_enabled_) {
		const data::decode::AnnotationClass *const ann_class = ann ?
			ann->row()->decoder()->get_ann_class_by_id(ann->ann_class_id()) : nullptr;
		const quintptr type_id = ann_class ? reinterpret_cast<quintptr>(ann_class) :
			sourceModel()->data(source_index, AnnotationTypeIdRole).toULongLong();
		if (!type_filter_.contains(type_id))
			return false;
	}

	if (search_text_.isEmpty())
		return true;

	const QString value = ann ? ann->longest_annotation() :
		sourceModel()->data(sourceModel()->index(sourceRow, 5, sourceParent),
			Qt::DisplayRole).toString();
	return value.contains(search_text_, Qt::CaseInsensitive);
}

void CustomFilterProxyModel::set_sample_range(uint64_t start_sample,
	uint64_t end_sample)
{
	range_start_sample_ = start_sample;
	range_end_sample_ = end_sample;

	invalidateFilter();
}

void CustomFilterProxyModel::set_search_text(const QString &text)
{
	search_text_ = text;
	invalidateFilter();
}

void CustomFilterProxyModel::set_group_filter(const QSet<quintptr> &groups,
	bool enabled)
{
	group_filter_ = groups;
	group_filtering_enabled_ = enabled;
	invalidateFilter();
}

void CustomFilterProxyModel::set_type_filter(const QSet<quintptr> &types,
	bool enabled)
{
	type_filter_ = types;
	type_filtering_enabled_ = enabled;
	invalidateFilter();
}

void CustomFilterProxyModel::enable_range_filtering(bool value)
{
	range_filtering_enabled_ = value;

	invalidateFilter();
}


QSize CustomTableView::minimumSizeHint() const
{
	QSize size(QTableView::sizeHint());

	int width = 0;
	for (int i = 0; i < horizontalHeader()->count(); i++)
		if (!horizontalHeader()->isSectionHidden(i))
			width += horizontalHeader()->sectionSize(i);

	size.setWidth(width + (horizontalHeader()->count() * 1));

	return size;
}

QSize CustomTableView::sizeHint() const
{
	return minimumSizeHint();
}

void CustomTableView::keyPressEvent(QKeyEvent *event)
{
	if ((event->key() == Qt::Key_Return) || (event->key() == Qt::Key_Enter))
		activatedByKey(currentIndex());
	else
		QTableView::keyPressEvent(event);
}


View::View(Session &session, bool is_main_view, QMainWindow *parent) :
	ViewBase(session, is_main_view, parent),

	// Note: Place defaults in View::reset_view_state(), not here
	parent_(parent),
	decoder_selector_(new QComboBox()),
	view_mode_selector_(new QComboBox()),
	search_edit_(new QLineEdit()),
	decoder_filter_button_(new QToolButton()),
	group_filter_button_(new QToolButton()),
	type_filter_button_(new QToolButton()),
	decoder_filter_menu_(new MultiSelectMenu(this)),
	group_filter_menu_(new MultiSelectMenu(this)),
	type_filter_menu_(new MultiSelectMenu(this)),
	save_button_(new QToolButton()),
	save_action_(new QAction(this)),
	table_view_(new CustomTableView()),
	model_(new AnnotationCollectionModel(this)),
	signal_(nullptr)
{
	QVBoxLayout *root_layout = new QVBoxLayout(this);
	root_layout->setContentsMargins(0, 0, 0, 0);
	root_layout->addWidget(table_view_);

	// Create toolbar
	QToolBar* toolbar = new QToolBar();
	toolbar->setContextMenuPolicy(Qt::PreventContextMenu);
	parent->addToolBar(toolbar);

	// Populate toolbar
	toolbar->addWidget(new QLabel(tr("Signal:")));
	toolbar->addWidget(decoder_selector_);
	toolbar->addSeparator();
	toolbar->addWidget(save_button_);
	toolbar->addSeparator();
	toolbar->addWidget(view_mode_selector_);
	toolbar->addSeparator();
	toolbar->addWidget(decoder_filter_button_);
	toolbar->addWidget(group_filter_button_);
	toolbar->addWidget(type_filter_button_);
	toolbar->addSeparator();
	toolbar->addWidget(search_edit_);

	connect(decoder_selector_, SIGNAL(currentIndexChanged(int)),
		this, SLOT(on_selected_decoder_changed(int)));
	connect(view_mode_selector_, SIGNAL(currentIndexChanged(int)),
		this, SLOT(on_view_mode_changed(int)));
	connect(search_edit_, &QLineEdit::textChanged,
		model_, &AnnotationCollectionModel::set_search_text);

	// Configure widgets
	decoder_selector_->setSizeAdjustPolicy(QComboBox::AdjustToContents);

	for (int i = 0; i < ViewModeCount; i++)
		view_mode_selector_->addItem(ViewModeNames[i], QVariant::fromValue(i));

	search_edit_->setClearButtonEnabled(true);
	search_edit_->setPlaceholderText(tr("Search values..."));
	search_edit_->setToolTip(tr("Search annotation values (Ctrl+F)"));
	search_edit_->setMinimumWidth(QFontMetrics(search_edit_->font())
		.horizontalAdvance(search_edit_->placeholderText()) * 2);

	decoder_filter_button_->setMenu(decoder_filter_menu_);
	decoder_filter_button_->setPopupMode(QToolButton::InstantPopup);
	group_filter_button_->setMenu(group_filter_menu_);
	group_filter_button_->setPopupMode(QToolButton::InstantPopup);
	type_filter_button_->setMenu(type_filter_menu_);
	type_filter_button_->setPopupMode(QToolButton::InstantPopup);

	QShortcut *find_shortcut = new QShortcut(QKeySequence::Find, this);
	connect(find_shortcut, &QShortcut::activated, search_edit_,
		qOverload<>(&QLineEdit::setFocus));

	// Configure actions
	save_action_->setText(tr("&Save..."));
	save_action_->setIcon(QIcon::fromTheme("document-save-as",
		QIcon(":/icons/document-save-as.png")));
	save_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));
	connect(save_action_, SIGNAL(triggered(bool)),
		this, SLOT(on_actionSave_triggered()));

	QMenu *save_menu = new QMenu();
	connect(save_menu, SIGNAL(triggered(QAction*)),
		this, SLOT(on_actionSave_triggered(QAction*)));

	for (int i = 0; i < SaveTypeCount; i++) {
		QAction *const action =	save_menu->addAction(tr(SaveTypeNames[i]));
		action->setData(QVariant::fromValue(i));
	}

	save_button_->setMenu(save_menu);
	save_button_->setDefaultAction(save_action_);
	save_button_->setPopupMode(QToolButton::MenuButtonPopup);

	// Set up the models and the table view
	table_view_->setModel(model_);
	model_->set_hide_hidden(true);

	table_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
	table_view_->setSelectionMode(QAbstractItemView::ContiguousSelection);
	// Decoder annotations already arrive in chronological order. Sorting the
	// potentially millions of low-level annotations makes every filter change
	// rebuild an N log N proxy mapping and can stall the UI for minutes.
	table_view_->setSortingEnabled(false);

	for (uint8_t i = model_->first_hidden_column(); i < model_->columnCount(); i++)
		table_view_->setColumnHidden(i, true);

	const int font_height = QFontMetrics(QApplication::font()).height();
	table_view_->verticalHeader()->setDefaultSectionSize((font_height * 5) / 4);
	table_view_->verticalHeader()->setVisible(false);

	table_view_->horizontalHeader()->setStretchLastSection(true);
	table_view_->horizontalHeader()->setCascadingSectionResizes(true);
	table_view_->horizontalHeader()->setSectionsMovable(true);
	table_view_->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);

	table_view_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	parent->setSizePolicy(table_view_->sizePolicy());

	connect(table_view_, SIGNAL(clicked(const QModelIndex&)),
		this, SLOT(on_table_item_clicked(const QModelIndex&)));
	connect(table_view_, SIGNAL(doubleClicked(const QModelIndex&)),
		this, SLOT(on_table_item_double_clicked(const QModelIndex&)));
	connect(table_view_, SIGNAL(activatedByKey(const QModelIndex&)),
		this, SLOT(on_table_item_double_clicked(const QModelIndex&)));
	connect(table_view_->horizontalHeader(), SIGNAL(customContextMenuRequested(const QPoint&)),
		this, SLOT(on_table_header_requested(const QPoint&)));

	// Set up metadata event handler
	session_.metadata_obj_manager()->add_observer(this);

	reset_view_state();
}

View::~View()
{
	session_.metadata_obj_manager()->remove_observer(this);
}

ViewType View::get_type() const
{
	return ViewTypeTabularDecoder;
}

void View::reset_view_state()
{
	ViewBase::reset_view_state();

	decoder_selector_->clear();
	search_edit_->clear();
	rebuild_annotation_filters();
}

void View::clear_decode_signals()
{
	ViewBase::clear_decode_signals();

	reset_data();
	reset_view_state();
}

void View::add_decode_signal(shared_ptr<data::DecodeSignal> signal)
{
	ViewBase::add_decode_signal(signal);

	connect(signal.get(), SIGNAL(name_changed(const QString&)),
		this, SLOT(on_signal_name_changed(const QString&)));

	// Note: At time of initial creation, decode signals have no decoders so we
	// need to watch for decoder stacking events

	connect(signal.get(), SIGNAL(decoder_stacked(void*)),
		this, SLOT(on_decoder_stacked(void*)));
	connect(signal.get(), SIGNAL(decoder_removed(void*)),
		this, SLOT(on_decoder_removed(void*)));

	// Add the top-level decoder provided by an already-existing signal
	auto stack = signal->decoder_stack();
	if (!stack.empty()) {
		shared_ptr<Decoder>& dec = stack.at(0);
		decoder_selector_->addItem(signal->name(), QVariant::fromValue((void*)dec.get()));
	}
}

void View::remove_decode_signal(shared_ptr<data::DecodeSignal> signal)
{
	// Remove all decoders provided by this signal
	for (const shared_ptr<Decoder>& dec : signal->decoder_stack()) {
		int index = decoder_selector_->findData(QVariant::fromValue((void*)dec.get()));

		if (index != -1)
			decoder_selector_->removeItem(index);
	}

	ViewBase::remove_decode_signal(signal);

	if (signal.get() == signal_) {
		reset_data();
		update_data();
		reset_view_state();
	}
}

void View::save_settings(QSettings &settings) const
{
	ViewBase::save_settings(settings);

	settings.setValue("view_mode", view_mode_selector_->currentIndex());
}

void View::restore_settings(QSettings &settings)
{
	ViewBase::restore_settings(settings);

	if (settings.contains("view_mode"))
		view_mode_selector_->setCurrentIndex(settings.value("view_mode").toInt());
}

void View::reset_data()
{
	signal_ = nullptr;
	decoder_ = nullptr;
}

void View::update_data()
{
	model_->set_signal_and_segment(signal_, current_segment_);
}

void View::rebuild_annotation_filters()
{
	available_decoders_.clear();
	available_groups_.clear();
	available_types_.clear();
	selected_decoders_.clear();
	selected_groups_.clear();
	selected_types_.clear();

	if (signal_) {
		for (const shared_ptr<Decoder>& decoder : signal_->decoder_stack()) {
			if (!decoder->visible())
				continue;
			const QString decoder_name = QString::fromUtf8(decoder->name());
			const quintptr decoder_id = reinterpret_cast<quintptr>(decoder.get());
			available_decoders_.push_back(
				{decoder_name, decoder_name, decoder_id});
			selected_decoders_.insert(decoder_id);
			for (const data::decode::Row *row : decoder->get_rows())
				if (row->visible()) {
					const quintptr id = reinterpret_cast<quintptr>(row);
					available_groups_.push_back({decoder_name, row->description(), id});
					selected_groups_.insert(id);
				}
			for (const data::decode::AnnotationClass *ann_class : decoder->ann_classes())
				if (ann_class->visible() && ann_class->row->visible()) {
					const quintptr id = reinterpret_cast<quintptr>(ann_class);
					available_types_.push_back({decoder_name,
						QString::fromUtf8(ann_class->description), id});
					selected_types_.insert(id);
				}
		}
	}

	rebuild_decoder_filter_menu();
	rebuild_filter_menu(group_filter_menu_, available_groups_, true);
	rebuild_filter_menu(type_filter_menu_, available_types_, false);
	apply_decoder_filter();
	apply_group_filter();
	apply_type_filter();
}

void View::rebuild_decoder_filter_menu()
{
	decoder_filter_menu_->clear();

	QAction *const select_all = decoder_filter_menu_->addAction(tr("Select all"));
	QAction *const clear = decoder_filter_menu_->addAction(tr("Clear"));
	decoder_filter_menu_->addSeparator();

	connect(select_all, &QAction::triggered, this, [this]() {
		selected_decoders_.clear();
		for (const FilterItem &item : available_decoders_)
			selected_decoders_.insert(item.id);
		for (QAction *action : decoder_filter_menu_->actions())
			if (action->isCheckable()) {
				const QSignalBlocker blocker(action);
				action->setChecked(true);
			}
		apply_decoder_filter();
	});
	connect(clear, &QAction::triggered, this, [this]() {
		selected_decoders_.clear();
		for (QAction *action : decoder_filter_menu_->actions())
			if (action->isCheckable()) {
				const QSignalBlocker blocker(action);
				action->setChecked(false);
			}
		apply_decoder_filter();
	});

	for (const FilterItem &item : available_decoders_) {
		QAction *const action = decoder_filter_menu_->addAction(item.label);
		action->setCheckable(true);
		action->setChecked(selected_decoders_.contains(item.id));
		action->setData(QVariant::fromValue<qulonglong>(item.id));
		connect(action, &QAction::toggled, this,
			[this, id=item.id](bool checked) {
				if (checked)
					selected_decoders_.insert(id);
				else
					selected_decoders_.remove(id);
				apply_decoder_filter();
			});
	}
}

void View::rebuild_filter_menu(QMenu *menu, const vector<FilterItem> &items,
	bool groups)
{
	menu->clear();

	QAction *const select_all = menu->addAction(tr("Select all"));
	QAction *const clear = menu->addAction(tr("Clear"));
	menu->addSeparator();

	connect(select_all, &QAction::triggered, this,
		[this, menu, groups]() {
			set_all_filter_items(menu, true, groups);
		});
	connect(clear, &QAction::triggered, this,
		[this, menu, groups]() {
			set_all_filter_items(menu, false, groups);
		});

	QString current_decoder;
	for (const FilterItem &item : items) {
		if (item.decoder_name != current_decoder) {
			current_decoder = item.decoder_name;
			menu->addSection(current_decoder);
		}

		QAction *const action = menu->addAction(
			item.label.isEmpty() ? tr("(unnamed)") : item.label);
		action->setCheckable(true);
		action->setChecked(groups ? selected_groups_.contains(item.id) :
			selected_types_.contains(item.id));
		action->setData(QVariant::fromValue<qulonglong>(item.id));
		action->setProperty("decoder_name", item.decoder_name);
		connect(action, &QAction::toggled, this,
			[this, id=item.id, groups](bool checked) {
			QSet<quintptr> &selected = groups ? selected_groups_ : selected_types_;
			if (checked)
				selected.insert(id);
			else
				selected.remove(id);
			if (groups)
				apply_group_filter();
			else
				apply_type_filter();
		});
	}
}

void View::set_all_filter_items(QMenu *menu, bool checked, bool groups)
{
	QSet<quintptr> &selected = groups ? selected_groups_ : selected_types_;
	const vector<FilterItem> &available = groups ? available_groups_ : available_types_;
	selected.clear();
	if (checked)
		for (const FilterItem &item : available)
			selected.insert(item.id);
	for (QAction *action : menu->actions()) {
		if (!action->isCheckable())
			continue;
		const QSignalBlocker blocker(action);
		action->setChecked(checked);
	}
	if (groups)
		apply_group_filter();
	else
		apply_type_filter();
}

void View::apply_group_filter()
{
	group_filter_button_->setText(filter_button_text(
		tr("Groups"), selected_groups_.size(),
		static_cast<qsizetype>(available_groups_.size())));
	group_filter_button_->setEnabled(!available_groups_.empty());
	model_->set_group_filter(selected_groups_,
		selected_groups_.size() != static_cast<qsizetype>(available_groups_.size()));
}

void View::apply_decoder_filter()
{
	decoder_filter_button_->setText(filter_button_text(
		tr("Decoders"), selected_decoders_.size(),
		static_cast<qsizetype>(available_decoders_.size())));
	decoder_filter_button_->setEnabled(!available_decoders_.empty());
	model_->set_decoder_filter(selected_decoders_,
		selected_decoders_.size() !=
		static_cast<qsizetype>(available_decoders_.size()));
}

void View::apply_type_filter()
{
	type_filter_button_->setText(filter_button_text(
		tr("Types"), selected_types_.size(),
		static_cast<qsizetype>(available_types_.size())));
	type_filter_button_->setEnabled(!available_types_.empty());
	model_->set_type_filter(selected_types_,
		selected_types_.size() != static_cast<qsizetype>(available_types_.size()));
}

void View::save_data_as_csv(unsigned int save_type) const
{
	// Note: We try to follow RFC 4180 (https://tools.ietf.org/html/rfc4180)

	assert(decoder_);
	assert(signal_);

	if (!signal_)
		return;

	const bool save_all = !table_view_->selectionModel()->hasSelection();

	GlobalSettings settings;
	const QString dir = settings.value("MainWindow/SaveDirectory").toString();

	const QString file_name = QFileDialog::getSaveFileName(
		parent_, tr("Save Annotations as CSV"), dir, tr("CSV Files (*.csv);;Text Files (*.txt);;All Files (*)"));

	if (file_name.isEmpty())
		return;

	QFile file(file_name);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		QTextStream out_stream(&file);

		if (save_all)
			table_view_->selectAll();

		// Write out header columns in visual order, not logical order
		for (int i = 0; i < table_view_->horizontalHeader()->count(); i++) {
			int column = table_view_->horizontalHeader()->logicalIndex(i);

			if (table_view_->horizontalHeader()->isSectionHidden(column))
				continue;

			const QString title = model_->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();

			if (save_type == SaveTypeCSVEscaped)
				out_stream << title;
			else
				out_stream << '"' << title << '"';

			if (i < (table_view_->horizontalHeader()->count() - 1))
				out_stream << ",";
		}
		out_stream << '\r' << '\n';


		QModelIndexList selected_rows = table_view_->selectionModel()->selectedRows();

		for (int i = 0; i < selected_rows.size(); i++) {
			const int row = selected_rows.at(i).row();

			// Write out columns in visual order, not logical order
			for (int c = 0; c < table_view_->horizontalHeader()->count(); c++) {
				const int column = table_view_->horizontalHeader()->logicalIndex(c);

				if (table_view_->horizontalHeader()->isSectionHidden(column))
					continue;

				const QModelIndex idx = model_->index(row, column);
				QString s = model_->data(idx, Qt::DisplayRole).toString();

				if (save_type == SaveTypeCSVEscaped)
					out_stream << s.replace(",", "\\,");
				else
					out_stream << '"' << s.replace("\"", "\"\"") << '"';

				if (c < (table_view_->horizontalHeader()->count() - 1))
					out_stream << ",";
			}

			out_stream << '\r' << '\n';
		}

		if (out_stream.status() == QTextStream::Ok) {
			if (save_all)
				table_view_->clearSelection();

			return;
		}
	}

	QMessageBox msg(parent_);
	msg.setText(tr("Error") + "\n\n" + tr("File %1 could not be written to.").arg(file_name));
	msg.setStandardButtons(QMessageBox::Ok);
	msg.setIcon(QMessageBox::Warning);
	msg.exec();
}

void View::on_selected_decoder_changed(int index)
{
	if (signal_) {
		disconnect(signal_, SIGNAL(color_changed(QColor)));
		disconnect(signal_, SIGNAL(new_annotations()));
		disconnect(signal_, SIGNAL(decode_reset()));
	}

	reset_data();

	decoder_ = (Decoder*)decoder_selector_->itemData(index).value<void*>();

	// Find the signal that contains the selected decoder
	for (const shared_ptr<DecodeSignal>& ds : decode_signals_)
		for (const shared_ptr<Decoder>& dec : ds->decoder_stack())
			if (decoder_ == dec.get())
				signal_ = ds.get();

	if (signal_) {
		connect(signal_, SIGNAL(color_changed(QColor)), this, SLOT(on_signal_color_changed(QColor)));
		connect(signal_, SIGNAL(new_annotations()), this, SLOT(on_new_annotations()));
		connect(signal_, SIGNAL(decode_reset()), this, SLOT(on_decoder_reset()));
	}

	rebuild_annotation_filters();
	update_data();

	// Force repaint, otherwise the new selection isn't shown for some reason
	table_view_->viewport()->update();
}

void View::on_view_mode_changed(int index)
{
	if (index == ViewModeAll)
		model_->enable_range_filtering(false);

	if (index == ViewModeVisible) {
		MetadataObject *md_obj =
			session_.metadata_obj_manager()->find_object_by_type(MetadataObjMainViewRange);
		assert(md_obj);

		int64_t start_sample = md_obj->value(MetadataValueStartSample).toLongLong();
		int64_t end_sample = md_obj->value(MetadataValueEndSample).toLongLong();

		model_->set_sample_range(max((int64_t)0, start_sample),
			max((int64_t)0, end_sample));
		model_->enable_range_filtering(true);
	}

	if (index == ViewModeLatest) {
		model_->enable_range_filtering(false);

		table_view_->scrollTo(
			model_->index(model_->rowCount() - 1, 0),
			QAbstractItemView::PositionAtBottom);
	}
}

void View::on_signal_name_changed(const QString &name)
{
	(void)name;

	SignalBase* sb = qobject_cast<SignalBase*>(QObject::sender());
	assert(sb);

	DecodeSignal* signal = dynamic_cast<DecodeSignal*>(sb);
	assert(signal);

	// Update the top-level decoder provided by this signal
	auto stack = signal->decoder_stack();
	if (!stack.empty()) {
		shared_ptr<Decoder>& dec = stack.at(0);
		int index = decoder_selector_->findData(QVariant::fromValue((void*)dec.get()));

		if (index != -1)
			decoder_selector_->setItemText(index, signal->name());
	}
}

void View::on_signal_color_changed(const QColor &color)
{
	(void)color;

	// Force immediate repaint, otherwise it's updated after the header popup is closed
	table_view_->viewport()->update();
}

void View::on_new_annotations()
{
	if (view_mode_selector_->currentIndex() == ViewModeLatest) {
		update_data();
		table_view_->scrollTo(
			model_->index(model_->rowCount() - 1, 0),
			QAbstractItemView::PositionAtBottom);
	} else {
		if (!delayed_view_updater_.isActive())
			delayed_view_updater_.start();
	}
}

void View::on_decoder_reset()
{
	// Invalidate the model's data connection immediately - otherwise we
	// will use a stale pointer in model_->index() when called from the table view
	model_->set_signal_and_segment(signal_, current_segment_);
}

void View::on_decoder_stacked(void* decoder)
{
	Decoder* d = static_cast<Decoder*>(decoder);

	// Find the signal that contains the selected decoder
	DecodeSignal* signal = nullptr;

	for (const shared_ptr<DecodeSignal>& ds : decode_signals_)
		for (const shared_ptr<Decoder>& dec : ds->decoder_stack())
			if (d == dec.get())
				signal = ds.get();

	assert(signal);

	const shared_ptr<Decoder>& dec = signal->decoder_stack().at(0);
	int index = decoder_selector_->findData(QVariant::fromValue((void*)dec.get()));

	if (index == -1) {
		// Add the decoder to the list
		decoder_selector_->addItem(signal->name(), QVariant::fromValue((void*)d));
	}

	if (signal == signal_)
		rebuild_annotation_filters();
}

void View::on_decoder_removed(void* decoder)
{
	Decoder* d = static_cast<Decoder*>(decoder);

	// Remove the decoder from the list
	int index = decoder_selector_->findData(QVariant::fromValue((void*)d));

	if (index != -1)
		decoder_selector_->removeItem(index);

	rebuild_annotation_filters();
}

void View::on_actionSave_triggered(QAction* action)
{
	int save_type = SaveTypeCSVQuoted;

	if (action)
		save_type = action->data().toInt();

	save_data_as_csv(save_type);
}

void View::on_table_item_clicked(const QModelIndex& index)
{
	(void)index;

	// Force repaint, otherwise the new selection isn't shown for some reason
	table_view_->viewport()->update();
}

void View::on_table_item_double_clicked(const QModelIndex& index)
{
	const Annotation* ann = static_cast<const Annotation*>(index.internalPointer());
	assert(ann);

	shared_ptr<views::ViewBase> main_view = session_.main_view();

	main_view->focus_on_range(ann->start_sample(), ann->end_sample());
}

void View::on_table_header_requested(const QPoint& pos)
{
	QMenu* menu = new QMenu(this);

	for (int i = 0; i < table_view_->horizontalHeader()->count(); i++) {
		int column = table_view_->horizontalHeader()->logicalIndex(i);

		const QString title =
			model_->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
		QAction* action = new QAction(title, this);

		action->setCheckable(true);
		action->setChecked(!table_view_->horizontalHeader()->isSectionHidden(column));
		action->setData(column);

		connect(action, SIGNAL(toggled(bool)), this, SLOT(on_table_header_toggled(bool)));

		menu->addAction(action);
	}

	menu->popup(table_view_->horizontalHeader()->viewport()->mapToGlobal(pos));
}

void View::on_table_header_toggled(bool checked)
{
	QAction* action = qobject_cast<QAction*>(QObject::sender());
	assert(action);

	const int column = action->data().toInt();

	table_view_->horizontalHeader()->setSectionHidden(column, !checked);
}

void View::on_metadata_object_changed(MetadataObject* obj,
	MetadataValueType value_type)
{
	// Check if we need to update the model's data range. We only work on the
	// end sample value because the start sample value is updated first and
	// we don't want to update the model twice
	if ((view_mode_selector_->currentIndex() == ViewModeVisible) &&
		(obj->type() == MetadataObjMainViewRange) &&
		(value_type == MetadataValueEndSample)) {

		int64_t start_sample = obj->value(MetadataValueStartSample).toLongLong();
		int64_t end_sample = obj->value(MetadataValueEndSample).toLongLong();

		model_->set_sample_range(max((int64_t)0, start_sample),
			max((int64_t)0, end_sample));
	}

	if (obj->type() == MetadataObjMousePos) {
		QModelIndex first_visible_idx = model_->index(0, 0);
		QModelIndex last_visible_idx = model_->index(model_->rowCount() - 1, 0);

		if (first_visible_idx.isValid()) {
			const QModelIndex first_highlighted_idx =
				model_->update_highlighted_rows(first_visible_idx, last_visible_idx,
					obj->value(MetadataValueStartSample).toLongLong());

			if (view_mode_selector_->currentIndex() == ViewModeVisible) {
				table_view_->scrollTo(first_highlighted_idx,
					QAbstractItemView::EnsureVisible);
			}

			// Force repaint, otherwise the table doesn't immediately update for some reason
			table_view_->viewport()->update();
		}
	}
}

void View::perform_delayed_view_update()
{
	update_data();
}


} // namespace tabular_decoder
} // namespace views
} // namespace pv
