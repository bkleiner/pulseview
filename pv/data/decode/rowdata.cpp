/*
 * This file is part of the PulseView project.
 *
 * Copyright (C) 2014 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#include <algorithm>
#include <cassert>

#include <pv/data/decode/decoder.hpp>
#include <pv/data/decode/row.hpp>
#include <pv/data/decode/rowdata.hpp>

using std::vector;

namespace pv {
namespace data {
namespace decode {

static const size_t AnnotationIndexBlockSize = 256;

RowData::RowData(Row* row) :
	row_(row),
	prev_ann_start_sample_(0)
{
	assert(row);
}

const Row* RowData::row() const
{
	return row_;
}

uint64_t RowData::get_max_sample() const
{
	if (annotations_.empty())
		return 0;
	return annotations_.back().end_sample();
}

uint64_t RowData::get_annotation_count() const
{
	return annotations_.size();
}

void RowData::get_annotation_subset(
	deque<const pv::data::decode::Annotation*> &dest,
	uint64_t start_sample, uint64_t end_sample, uint64_t max_annotations) const
{
	// Determine whether we must apply per-class filtering or not
	bool all_ann_classes_enabled = true;
	bool all_ann_classes_disabled = true;

	uint32_t max_ann_class_id = 0;
	for (AnnotationClass* c : row_->ann_classes()) {
		if (!c->visible())
			all_ann_classes_enabled = false;
		else
			all_ann_classes_disabled = false;
		if (c->id > max_ann_class_id)
			max_ann_class_id = c->id;
	}

	if (all_ann_classes_disabled || annotations_.empty())
		return;

	// The annotations are ordered by start sample. A prefix maximum of their end
	// samples, stored once per block, lets us skip annotations before the
	// requested range without losing a long annotation that overlaps it.
	const auto max_end_it = std::upper_bound(annotation_block_max_ends_.begin(),
		annotation_block_max_ends_.end(), start_sample);
	const size_t first_block = std::distance(
		annotation_block_max_ends_.begin(), max_end_it);
	const size_t first_index = std::min(first_block * AnnotationIndexBlockSize,
		annotations_.size());
	const auto first = annotations_.begin() + first_index;
	const auto last = std::upper_bound(first, annotations_.end(), end_sample,
		[](uint64_t sample, const Annotation& annotation) {
			return sample < annotation.start_sample();
		});

	if (all_ann_classes_enabled) {
		const uint64_t count = std::distance(first, last);
		const uint64_t stride = (max_annotations && count > max_annotations) ?
			(count + max_annotations - 1) / max_annotations : 1;

		for (auto it = first; it < last; it += stride)
			if (it->end_sample() > start_sample)
				dest.push_back(&(*it));
	} else {
		// Filter out invisible annotation classes. Mixed visibility is uncommon,
		// so retain every visible annotation rather than sampling away a class.
		vector<size_t> class_visible(max_ann_class_id + 1, 0);
		for (AnnotationClass* c : row_->ann_classes())
			if (c->visible())
				class_visible[c->id] = 1;

		for (auto it = first; it < last; ++it)
			if (it->end_sample() > start_sample &&
				class_visible[it->ann_class_id()])
				dest.push_back(&(*it));
	}
}

const deque<Annotation>& RowData::annotations() const
{
	return annotations_;
}

const Annotation* RowData::emplace_annotation(srd_proto_data *pdata)
{
	const srd_proto_data_annotation *const pda = (const srd_proto_data_annotation*)pdata->data;

	uint32_t ann_class_id = pda->ann_class;

	// Look up the longest annotation text to see if we have it in storage.
	// This implies that if the longest text is the same, the shorter texts
	// are expected to be the same, too. PDs that violate this assumption
	// should be considered broken.
	const char* const* ann_texts = (char**)pda->ann_text;
	const QString ann0 = QString::fromUtf8(ann_texts[0]);
	vector<QString>* storage_entry = &(ann_texts_[ann0]);

	if (storage_entry->empty()) {
		while (*ann_texts) {
			storage_entry->emplace_back(QString::fromUtf8(*ann_texts));
			ann_texts++;
		}
		storage_entry->shrink_to_fit();
	}


	const Annotation* result = nullptr;

	// We insert the annotation in a way so that the annotation list
	// is sorted by start sample. Otherwise, we'd have to sort when
	// painting, which is expensive

	if (pdata->start_sample < prev_ann_start_sample_) {
		// Find location to insert the annotation at

		auto it = annotations_.end();
		do {
			it--;
		} while ((it->start_sample() > pdata->start_sample) && (it != annotations_.begin()));

		// Allow inserting at the front
		if (it != annotations_.begin())
			it++;

		it = annotations_.emplace(it, pdata->start_sample, pdata->end_sample,
			storage_entry, ann_class_id, this);
		result = &(*it);

		const size_t index = std::distance(annotations_.begin(), it);
		const size_t first_changed_block = index / AnnotationIndexBlockSize;
		const size_t block_count =
			(annotations_.size() + AnnotationIndexBlockSize - 1) /
			AnnotationIndexBlockSize;
		annotation_block_max_ends_.resize(block_count);
		uint64_t max_end = first_changed_block ?
			annotation_block_max_ends_[first_changed_block - 1] : 0;
		for (size_t block = first_changed_block; block < block_count; block++) {
			const size_t block_end = std::min(
				(block + 1) * AnnotationIndexBlockSize, annotations_.size());
			for (size_t i = block * AnnotationIndexBlockSize; i < block_end; i++)
				max_end = std::max(max_end, annotations_[i].end_sample());
			annotation_block_max_ends_[block] = max_end;
		}
	} else {
		annotations_.emplace_back(pdata->start_sample, pdata->end_sample,
			storage_entry, ann_class_id, this);
		result = &(annotations_.back());
		prev_ann_start_sample_ = pdata->start_sample;
		const size_t block = (annotations_.size() - 1) / AnnotationIndexBlockSize;
		if (block == annotation_block_max_ends_.size())
			annotation_block_max_ends_.push_back(std::max(pdata->end_sample,
				annotation_block_max_ends_.empty() ? uint64_t(0) :
				annotation_block_max_ends_.back()));
		else
			annotation_block_max_ends_.back() = std::max(
				annotation_block_max_ends_.back(), pdata->end_sample);
	}

	return result;
}

}  // namespace decode
}  // namespace data
}  // namespace pv
