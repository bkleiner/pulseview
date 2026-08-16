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

#ifndef PULSEVIEW_PV_VIEWS_TABULAR_DECODER_VIEW_HPP
#define PULSEVIEW_PV_VIEWS_TABULAR_DECODER_VIEW_HPP

#include <QAction>
#include <QComboBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QToolButton>

#include "pv/metadata_obj.hpp"
#include "pv/views/viewbase.hpp"
#include "pv/data/decodesignal.hpp"

namespace pv {
class Session;

namespace views {

namespace tabular_decoder {

// When adding an entry here, don't forget to update SaveTypeNames as well
enum SaveType {
	SaveTypeCSVEscaped,
	SaveTypeCSVQuoted,
	SaveTypeCount  // Indicates how many save types there are, must always be last
};

// When adding an entry here, don't forget to update ViewModeNames as well
enum ViewModeType {
	ViewModeAll,
	ViewModeLatest,
	ViewModeVisible,
	ViewModeCount // Indicates how many view mode types there are, must always be last
};

extern const char* SaveTypeNames[SaveTypeCount];
extern const char* ViewModeNames[ViewModeCount];

enum AnnotationDataRole {
	AnnotationGroupIdRole = Qt::UserRole,
	AnnotationTypeIdRole,
	AnnotationDecoderIdRole
};


class AnnotationCollectionModel : public QAbstractTableModel
{
	Q_OBJECT

public:
	AnnotationCollectionModel(QObject* parent = nullptr);

	int get_hierarchy_level(const Annotation* ann) const;
	QVariant data_from_ann(const Annotation* ann, int index) const;
	QVariant data(const QModelIndex& index, int role) const override;
	Qt::ItemFlags flags(const QModelIndex& index) const override;

	uint8_t first_hidden_column() const;
	QVariant headerData(int section, Qt::Orientation orientation,
		int role = Qt::DisplayRole) const override;
	QModelIndex index(int row, int column,
		const QModelIndex& parent_idx = QModelIndex()) const override;

	QModelIndex parent(const QModelIndex& index) const override;

	int rowCount(const QModelIndex& parent_idx = QModelIndex()) const override;
	int columnCount(const QModelIndex& parent_idx = QModelIndex()) const override;

	void set_signal_and_segment(data::DecodeSignal* signal, uint32_t current_segment);
	void set_hide_hidden(bool hide_hidden);
	void set_search_text(const QString &text);
	void set_group_filter(const QSet<quintptr> &groups, bool enabled);
	void set_type_filter(const QSet<quintptr> &types, bool enabled);
	void set_decoder_filter(const QSet<quintptr> &decoders, bool enabled);
	void set_sample_range(uint64_t start_sample, uint64_t end_sample);
	void enable_range_filtering(bool enabled);

	void update_annotations_without_hidden(size_t start_index = 0);
	void refilter_annotations();
	QModelIndex update_highlighted_rows(QModelIndex first, QModelIndex last,
		int64_t sample_num);

private Q_SLOTS:
	void on_annotation_visibility_changed();

private:
	vector<QVariant> header_data_;
	const deque<const Annotation*>* all_annotations_;
	deque<const Annotation*> all_annotations_without_hidden_;
	const deque<const Annotation*>* dataset_;
	data::DecodeSignal* signal_;
	uint8_t first_hidden_column_;
	uint32_t prev_segment_;
	uint64_t prev_last_row_;
	size_t processed_annotation_count_;
	int64_t highlight_sample_num_;
	bool had_highlight_before_;
	bool hide_hidden_;
	QString search_text_;
	QSet<quintptr> group_filter_, type_filter_, decoder_filter_;
	uint64_t range_start_sample_, range_end_sample_;
	bool group_filtering_enabled_, type_filtering_enabled_;
	bool decoder_filtering_enabled_, range_filtering_enabled_;
};


class CustomFilterProxyModel : public QSortFilterProxyModel
{
	Q_OBJECT

public:
	CustomFilterProxyModel(QObject* parent = 0);

	void set_sample_range(uint64_t start_sample, uint64_t end_sample);
	void set_search_text(const QString &text);
	void set_group_filter(const QSet<quintptr> &groups, bool enabled);
	void set_type_filter(const QSet<quintptr> &types, bool enabled);

	void enable_range_filtering(bool value);

protected:
	bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
	uint64_t range_start_sample_, range_end_sample_;
	QString search_text_;
	QSet<quintptr> group_filter_, type_filter_;
	bool range_filtering_enabled_;
	bool group_filtering_enabled_, type_filtering_enabled_;
};


class CustomTableView : public QTableView
{
	Q_OBJECT

public:
	virtual QSize minimumSizeHint() const override;
	virtual QSize sizeHint() const override;

protected:
	virtual void keyPressEvent(QKeyEvent *event) override;

Q_SIGNALS:
	void activatedByKey(const QModelIndex &index);
};


class View : public ViewBase, public MetadataObjObserverInterface
{
	Q_OBJECT

public:
	explicit View(Session &session, bool is_main_view=false, QMainWindow *parent = nullptr);
	~View();

	virtual ViewType get_type() const;

	/**
	 * Resets the view to its default state after construction. It does however
	 * not reset the signal bases or any other connections with the session.
	 */
	virtual void reset_view_state();

	virtual void clear_decode_signals();
	virtual void add_decode_signal(shared_ptr<data::DecodeSignal> signal);
	virtual void remove_decode_signal(shared_ptr<data::DecodeSignal> signal);

	virtual void save_settings(QSettings &settings) const;
	virtual void restore_settings(QSettings &settings);

private:
	struct FilterItem {
		QString decoder_name;
		QString label;
		quintptr id;
	};

	void reset_data();
	void update_data();
	void rebuild_annotation_filters();
	void rebuild_decoder_filter_menu();
	void rebuild_filter_menu(QMenu *menu, const vector<FilterItem> &items, bool groups);
	void set_all_filter_items(QMenu *menu, bool checked, bool groups);
	void apply_group_filter();
	void apply_type_filter();
	void apply_decoder_filter();

	void save_data_as_csv(unsigned int save_type) const;

private Q_SLOTS:
	void on_selected_decoder_changed(int index);
	void on_view_mode_changed(int index);

	void on_signal_name_changed(const QString &name);
	void on_signal_color_changed(const QColor &color);
	void on_new_annotations();

	void on_decoder_reset();
	void on_decoder_stacked(void* decoder);
	void on_decoder_removed(void* decoder);

	void on_actionSave_triggered(QAction* action = nullptr);

	void on_table_item_clicked(const QModelIndex& index);
	void on_table_item_double_clicked(const QModelIndex& index);
	void on_table_header_requested(const QPoint& pos);
	void on_table_header_toggled(bool checked);

	virtual void on_metadata_object_changed(MetadataObject* obj,
		MetadataValueType value_type);

	virtual void perform_delayed_view_update();

private:
	QWidget* parent_;

	QComboBox* decoder_selector_;
	QComboBox* view_mode_selector_;
	QLineEdit* search_edit_;
	QToolButton* decoder_filter_button_;
	QToolButton* group_filter_button_;
	QToolButton* type_filter_button_;
	QMenu* decoder_filter_menu_;
	QMenu* group_filter_menu_;
	QMenu* type_filter_menu_;
	vector<FilterItem> available_decoders_;
	vector<FilterItem> available_groups_, available_types_;
	QSet<quintptr> selected_decoders_;
	QSet<quintptr> selected_groups_, selected_types_;

	QToolButton* save_button_;
	QAction* save_action_;

	CustomTableView* table_view_;
	AnnotationCollectionModel* model_;

	data::DecodeSignal* signal_;
	const data::decode::Decoder* decoder_;
};

} // namespace tabular_decoder
} // namespace views
} // namespace pv

#endif // PULSEVIEW_PV_VIEWS_TABULAR_DECODER_VIEW_HPP
