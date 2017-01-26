package com.yahoo.searchdefinition;

import com.yahoo.document.DataType;
import com.yahoo.document.ReferenceDataType;
import com.yahoo.document.TemporaryStructuredDataType;
import com.yahoo.searchdefinition.document.SDDocumentType;
import com.yahoo.searchdefinition.document.SDField;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.ExpectedException;

import java.util.Map;

import static java.util.Arrays.asList;
import static java.util.Collections.singletonList;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

/**
 * @author bjorncs
 */
public class DocumentReferenceResolverTest {

    @Rule
    public final ExpectedException exceptionRule = ExpectedException.none();

    @Test
    public void reference_from_one_document_to_another_is_resolved() {
        // Create bar document with no fields
        Search barSearch = new Search();
        SDDocumentType barDocument = new SDDocumentType("bar", barSearch);
        barSearch.addDocument(barDocument);

        // Create foo document with document reference to bar and add another field
        SDField fooRefToBarField = new SDField
                ("bar_ref", ReferenceDataType.createWithInferredId(barDocument.getDocumentType()));
        SDField irrelevantField = new SDField("irrelevant_stuff", DataType.INT);
        Search fooSearch = new Search();
        SDDocumentType fooDocument = new SDDocumentType("foo", fooSearch);
        fooDocument.addField(fooRefToBarField);
        fooDocument.addField(irrelevantField);
        fooSearch.addDocument(fooDocument);

        DocumentReferenceResolver resolver = new DocumentReferenceResolver(asList(fooSearch, barSearch));
        resolver.resolveReferences(fooDocument);
        assertTrue(fooDocument.getDocumentReferences().isPresent());

        Map<String, DocumentReference> fooReferenceMap = fooDocument.getDocumentReferences().get().referenceMap();
        assertEquals(1, fooReferenceMap.size());
        assertSame(barSearch, fooReferenceMap.get("bar_ref").search());
        assertSame(fooRefToBarField, fooReferenceMap.get("bar_ref").documentReferenceField());
    }

    @Test
    public void throws_user_friendly_exception_if_referenced_document_does_not_exist() {
        // Create foo document with document reference to non-existing document bar
        SDField fooRefToBarField = new SDField(
                "bar_ref", ReferenceDataType.createWithInferredId(TemporaryStructuredDataType.create("bar")));
        Search fooSearch = new Search();
        SDDocumentType fooDocument = new SDDocumentType("foo", fooSearch);
        fooDocument.addField(fooRefToBarField);
        fooSearch.addDocument(fooDocument);

        DocumentReferenceResolver resolver = new DocumentReferenceResolver(singletonList(fooSearch));

        exceptionRule.expect(IllegalArgumentException.class);
        exceptionRule.expectMessage(
                "The field 'bar_ref' is an invalid document reference. " +
                        "Could not find document with 'bar' in any search definitions");
        resolver.resolveReferences(fooDocument);
    }

}