/* global describe it cy beforeEach require expect */

var helper = require('../../common/helper');
var calcHelper = require('../../common/calc_helper');

describe(['tagdesktop'], 'Calc styled table resize handle.', function() {

	beforeEach(function() {
		helper.setupAndLoadDocument('calc/table_resize_handles.xlsx');
	});

	// The corner resize handle is shown for every visible styled table, not only
	// the one under the cursor. With the cursor at A1 (in neither table) both
	// tables must still show a handle.
	it('shows the resize handle for every visible table, not just the one under the cursor', function() {
		calcHelper.clickOnFirstCell();

		// Two visible tables -> exactly two handle sections, indexes 0 and 1
		// (one per table), and no third.
		cy.cGet('[id="test-div-table range handle 0"]').should('exist');
		cy.cGet('[id="test-div-table range handle 1"]').should('exist');
		cy.cGet('[id="test-div-table range handle 2"]').should('not.exist');
	});

	// Both tables have empty rows below, so the grow can't be refused. Invoke the
	// section's handlers directly like the autofill spec.
	it('resizing a table by dragging its handle down grows the table', function() {
		calcHelper.clickOnFirstCell();
		cy.cGet('[id="test-div-table range handle 0"]').should('exist');

		var startY;
		cy.getFrameWindow().then(function(win) {
			var handle = win.app.sectionContainer.getSectionWithName('table range handle 0');
			expect(handle, 'table resize handle section').to.exist;
			startY = handle.position[1];

			var halfWidth = Math.floor(handle.size[0] / 2);
			var halfHeight = Math.floor(handle.size[1] / 2);
			var drop = win.app.calc.cellCursorRectangle.pHeight * 5;

			var localStart = win.cool.SimplePoint.fromCorePixels([halfWidth, halfHeight]);
			handle.onMouseDown(localStart, new win.MouseEvent('mousedown', { button: 0 }));

			var localEnd = win.cool.SimplePoint.fromCorePixels([halfWidth, halfHeight + drop]);
			handle.onMouseMove(localEnd, [0, drop], new win.MouseEvent('mousemove'));
			handle.onMouseUp(localEnd, new win.MouseEvent('mouseup', { button: 0 }));
		});

		cy.getFrameWindow().then(function(win) {
			helper.processToIdle(win);
		});

		cy.getFrameWindow().then(function(win) {
			var handle = win.app.sectionContainer.getSectionWithName('table range handle 0');
			expect(handle, 'table resize handle section').to.exist;
			expect(handle.position[1], 'handle moved down after grow').to.be.greaterThan(startY);
		});
	});
});
