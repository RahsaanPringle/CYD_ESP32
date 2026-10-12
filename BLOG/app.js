/* global Isotope */
(function ($) {
    'use strict';

    const dataPath = 'blog-data.json';
    let records = [];
    let isotope = null;
    let selectedIndex = null;

    const $form = $('#blog-form');
    const $saveButton = $('#save-button');
    const $refreshButton = $('#refresh-button');
    const $status = $('#status');
    const $dataStatus = $('#data-status');
    const $grid = $('#post-grid');
    const $preview = $('#preview');
    const $contentEditor = $('#content-editor');
    const $browser = $('#post-browser');
    const $detail = $('#post-detail');
    const $editor = $('#editor-panel');

    function formatDateTimeLocal(date) {
        const offset = date.getTimezoneOffset() * 60000;
        return new Date(date.getTime() - offset).toISOString().slice(0, 16);
    }

    function validDate(value) {
        const date = new Date(value);
        return Number.isNaN(date.getTime()) ? null : date;
    }

    function formatDisplayDate(value) {
        const date = validDate(value);
        return date ? new Intl.DateTimeFormat(undefined, {
            year: 'numeric',
            month: 'long',
            day: 'numeric'
        }).format(date) : 'Date unavailable';
    }

    function monthKey(value) {
        const date = validDate(value);
        if (!date) return 'unknown';
        return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}`;
    }

    function monthLabel(key) {
        if (key === 'unknown') return 'Date unavailable';
        const [year, month] = key.split('-').map(Number);
        return new Intl.DateTimeFormat(undefined, {
            year: 'numeric',
            month: 'long'
        }).format(new Date(year, month - 1, 1));
    }

    function sanitizeHtml(html) {
        const fragment = document.createDocumentFragment();
        const parsed = new DOMParser().parseFromString(html || '', 'text/html');
        const allowedTags = new Set(['A', 'B', 'BR', 'EM', 'I', 'LI', 'OL', 'P', 'STRONG', 'U', 'UL']);

        function copySafeNodes(source, target) {
            source.childNodes.forEach((node) => {
                if (node.nodeType === Node.TEXT_NODE) {
                    target.append(document.createTextNode(node.textContent));
                    return;
                }
                if (node.nodeType !== Node.ELEMENT_NODE) return;
                if (!allowedTags.has(node.tagName)) {
                    copySafeNodes(node, target);
                    return;
                }
                const safeNode = document.createElement(node.tagName.toLowerCase());
                if (node.tagName === 'A') {
                    const href = node.getAttribute('href') || '';
                    if (/^(https?:|mailto:)/i.test(href)) {
                        safeNode.href = href;
                        safeNode.target = '_blank';
                        safeNode.rel = 'noopener noreferrer';
                    }
                }
                copySafeNodes(node, safeNode);
                target.append(safeNode);
            });
        }

        copySafeNodes(parsed.body, fragment);
        const container = document.createElement('div');
        container.append(fragment);
        return container.innerHTML;
    }

    function plainText(html) {
        const container = document.createElement('div');
        container.innerHTML = sanitizeHtml(html);
        return container.textContent.replace(/\s+/g, ' ').trim();
    }

    function normalizeRecords(data) {
        if (Array.isArray(data)) return data.filter((record) => record && typeof record === 'object');
        if (data && typeof data === 'object') return [data];
        return [];
    }

    async function readExistingData() {
        const response = await fetch(`${dataPath}?t=${Date.now()}`, {
            cache: 'no-store'
        });
        if (response.status === 404) return [];
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return normalizeRecords(await response.json());
    }

    async function writeRecords(nextRecords) {
        const response = await fetch(dataPath, {
            method: 'PUT',
            headers: {
                'Content-Type': 'application/json'
            },
            body: `${JSON.stringify(nextRecords, null, 2)}\n`
        });
        if (!response.ok && response.status !== 201 && response.status !== 204) throw new Error(`HTTP ${response.status}`);
    }

    function createPostCard(record, index) {
        const date = validDate(record.postDate || record.publishedAt);
        const timestamp = date ? date.getTime() : 0;
        const category = String(record.category || 'Uncategorized').trim() || 'Uncategorized';
        const month = monthKey(record.postDate || record.publishedAt);
        const excerpt = plainText(record.content || '').slice(0, 220) || 'No preview available.';
        const $item = $('<article>', {
            class: 'post-item col pb-3',
            'data-index': index,
            'data-category': category.toLocaleLowerCase(),
            'data-category-label': category,
            'data-month': month,
            'data-date': timestamp
        });
        const $card = $('<div>', {
            class: 'post-card'
        });
        const $button = $('<button>', {
            class: 'post-card-button',
            type: 'button',
            'aria-label': `Read ${record.title || `Post ${index + 1}`}`
        });
        $('<span>', {
            class: 'badge rounded-pill category-badge mb-3',
            text: category
        }).appendTo($button);
        $('<h3>', {
            class: 'post-title h5 fw-bold',
            text: record.title || `Post ${index + 1}`
        }).appendTo($button);
        $('<p>', {
            class: 'post-meta mb-2',
            text: `${monthLabel(month)} · ${formatDisplayDate(record.postDate || record.publishedAt)}`
        }).appendTo($button);
        $('<p>', {
            class: 'post-excerpt mb-0',
            text: excerpt
        }).appendTo($button);
        $button.on('click', () => showPost(index));
        return $item.append($card.append($button));
    }

    function rebuildFilters() {
        const categorySelection = $('#category-filter').val() || '*';
        const monthSelection = $('#month-filter').val() || '*';
        const categories = [...new Set(records.map((record) => String(record.category || 'Uncategorized').trim() || 'Uncategorized'))]
            .sort((a, b) => a.localeCompare(b));
        const months = [...new Set(records.map((record) => monthKey(record.postDate || record.publishedAt)))]
            .sort().reverse();

        $('#category-filter').empty().append($('<option>', {
            value: '*',
            text: 'All categories'
        }));
        $('#category-suggestions').empty();
        categories.forEach((category) => {
            $('<option>', {
                value: category.toLocaleLowerCase(),
                text: category
            }).appendTo('#category-filter');
            $('<option>', {
                value: category
            }).appendTo('#category-suggestions');
        });

        $('#month-filter').empty().append($('<option>', {
            value: '*',
            text: 'All months'
        }));
        months.forEach((month) => $('<option>', {
            value: month,
            text: monthLabel(month)
        }).appendTo('#month-filter'));

        if ($(`#category-filter option[value="${CSS.escape(categorySelection)}"]`).length) $('#category-filter').val(categorySelection);
        if ($(`#month-filter option[value="${CSS.escape(monthSelection)}"]`).length) $('#month-filter').val(monthSelection);
    }

    function initializeIsotope() {
        if (isotope) isotope.destroy();
        isotope = new Isotope($grid[0], {
            itemSelector: '.post-item',
            layoutMode: 'fitRows',
            percentPosition: true,
            getSortData: {
                category: '[data-category-label]',
                date: (element) => Number(element.dataset.date || 0)
            },
            sortBy: 'date',
            sortAscending: false,
            transitionDuration: '0.3s'
        });
        isotope.on('arrangeComplete', (filteredItems) => $('#empty-filter-state').toggleClass('d-none', filteredItems.length > 0));
        applyGridControls();
    }

    function applyGridControls() {
        if (!isotope) return;
        const category = $('#category-filter').val();
        const month = $('#month-filter').val();
        const order = $('#sort-posts').val();
        const sortBy = order === 'category' ? ['category', 'date'] : 'date';
        const sortAscending = order === 'date-asc' ? true : order === 'category' ? {
            category: true,
            date: false
        } : false;

        isotope.arrange({
            filter(element) {
                const categoryMatches = category === '*' || element.dataset.category === category;
                const monthMatches = month === '*' || element.dataset.month === month;
                return categoryMatches && monthMatches;
            },
            sortBy,
            sortAscending
        });

        const categoryText = category === '*' ? 'all categories' : $('#category-filter option:selected').text();
        const monthText = month === '*' ? 'all months' : $('#month-filter option:selected').text();
        $('#active-filters').text(`Showing ${categoryText} · ${monthText}`);
    }

    function displayExistingData(data) {
        records = normalizeRecords(data);
        $grid.empty();
        records.forEach((record, index) => $grid.append(createPostCard(record, index)));
        rebuildFilters();
        initializeIsotope();
        $dataStatus.text(records.length ? `${records.length} post${records.length === 1 ? '' : 's'} loaded from ${dataPath}.` : `No posts are stored in ${dataPath}.`);
        $preview.text(records.length ? JSON.stringify(records, null, 2) : 'No posts saved yet.');
        $('#empty-filter-state').toggleClass('d-none', records.length > 0);
    }

    async function loadExistingData() {
        $refreshButton.prop('disabled', true);
        $dataStatus.text('Loading...');
        try {
            displayExistingData(await readExistingData());
        } catch (error) {
            records = [];
            $grid.empty();
            $dataStatus.text(`Could not load published posts: ${error.message}.`);
        } finally {
            $refreshButton.prop('disabled', false);
        }
    }

    function showSection(section) {
        $browser.toggleClass('d-none', section !== 'browser');
        $detail.toggleClass('d-none', section !== 'detail');
        $editor.toggleClass('d-none', section !== 'editor');
        document.querySelector(section === 'browser' ? '#published-posts-heading' : section === 'detail' ? '#detail-title' : '#editor-heading')?.focus({
            preventScroll: true
        });
        window.scrollTo({
            top: 0,
            behavior: 'smooth'
        });
    }

    function showPost(index) {
        const record = records[index];
        if (!record) return;
        selectedIndex = index;
        $('#detail-title').text(record.title || `Post ${index + 1}`);
        $('#detail-meta').text(`${record.category || 'Uncategorized'} · ${formatDisplayDate(record.postDate || record.publishedAt)}`);
        $('#detail-content').html(sanitizeHtml(record.content || ''));
        showSection('detail');
    }

    function resetEditor() {
        $form[0].reset();
        $('#editing-index').val('');
        $('#post-date').val(formatDateTimeLocal(new Date()));
        $contentEditor.empty();
        $('#editor-mode-label').text('New post');
        $('#editor-heading').text('Write a blog post');
        $saveButton.text('Publish post');
        $status.empty();
    }

    function openNewPost() {
        selectedIndex = null;
        resetEditor();
        showSection('editor');
        $('#title').trigger('focus');
    }

    function openEditor(index) {
        const record = records[index];
        if (!record) return;
        $('#editing-index').val(index);
        $('#title').val(record.title || '');
        $('#category').val(record.category || '');
        const date = validDate(record.postDate || record.publishedAt) || new Date();
        $('#post-date').val(formatDateTimeLocal(date));
        $contentEditor.html(sanitizeHtml(record.content || ''));
        $('#editor-mode-label').text('Editing post');
        $('#editor-heading').text(record.title || `Post ${index + 1}`);
        $saveButton.text('Save changes');
        $status.empty();
        showSection('editor');
        $('#title').trigger('focus');
    }

    $('[data-command]').on('click', function () {
        $contentEditor.trigger('focus');
        document.execCommand($(this).data('command'), false);
    });

    $('#link-button').on('click', () => {
        const url = window.prompt('Enter the link URL');
        if (!url) return;
        $contentEditor.trigger('focus');
        document.execCommand('createLink', false, url);
    });

    $('#category-filter, #month-filter, #sort-posts').on('change', applyGridControls);
    $refreshButton.on('click', loadExistingData);
    $('#new-post-button').on('click', openNewPost);
    $('#edit-post-button').on('click', () => openEditor(selectedIndex));
    $('#back-to-grid-button').on('click', () => showSection('browser'));
    $('#cancel-button, #cancel-edit-button').on('click', () => selectedIndex === null ? showSection('browser') : showPost(selectedIndex));

    $form.on('submit', async function (event) {
        event.preventDefault();
        const content = sanitizeHtml($contentEditor.html());
        if (!$contentEditor.text().trim()) {
            $status.text('Post content is required.');
            $contentEditor.trigger('focus');
            return;
        }

        const editingValue = $('#editing-index').val();
        const editingIndex = editingValue === '' ? null : Number(editingValue);
        const now = new Date().toISOString();
        const record = {
            ...(editingIndex === null ? {} : records[editingIndex]),
            title: $('#title').val().trim(),
            category: $('#category').val().trim(),
            postDate: new Date($('#post-date').val()).toISOString(),
            content,
            ...(editingIndex === null ? {
                publishedAt: now
            } : {
                updatedAt: now
            })
        };

        $saveButton.prop('disabled', true);
        $status.text(editingIndex === null ? 'Publishing...' : 'Saving changes...');

        try {
            const latestRecords = await readExistingData();
            if (editingIndex === null) {
                latestRecords.push(record);
                selectedIndex = latestRecords.length - 1;
            } else {
                latestRecords[editingIndex] = record;
                selectedIndex = editingIndex;
            }
            await writeRecords(latestRecords);
            displayExistingData(latestRecords);
            $status.text(editingIndex === null ? `Published to ${dataPath}.` : `Updated ${dataPath}.`);
            showPost(selectedIndex);
        } catch (error) {
            $status.text(`Save failed: ${error.message}. Check the WebDAV login and connection.`);
        } finally {
            $saveButton.prop('disabled', false);
        }
    });

    resetEditor();
    loadExistingData();
}(jQuery));